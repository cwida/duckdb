#include "duckdb/storage/statistics/distinct_statistics.hpp"

#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

DistinctStatistics::DistinctStatistics() : log(make_uniq<HyperLogLog>()), sample_count(0), total_count(0) {
}

DistinctStatistics::DistinctStatistics(unique_ptr<HyperLogLog> log, idx_t sample_count, idx_t total_count)
    : log(std::move(log)), sample_count(sample_count), total_count(total_count) {
}

unique_ptr<DistinctStatistics> DistinctStatistics::Copy() const {
	return make_uniq<DistinctStatistics>(log->Copy(), sample_count, total_count);
}

static SelectionVector CreateSampleSelection(idx_t sample_count) {
	SelectionVector result(sample_count);
	RandomEngine random(0);
	for (idx_t i = 0; i < sample_count; i++) {
		const auto begin = i * STANDARD_VECTOR_SIZE / sample_count;
		const auto end = (i + 1) * STANDARD_VECTOR_SIZE / sample_count;
		result.set_index(i, begin + random.NextRandomInteger64() % (end - begin));
	}
	return result;
}

template <class T>
static void UpdateSampleValues(HyperLogLog &log, const Vector &input, const SelectionVector &selection, idx_t count) {
	auto values = input.Values<T>();
	if (values.CanHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			auto entry = values[selection.get_index(i)];
			if (entry.IsValid()) {
				log.InsertElement(duckdb::Hash<T>(entry.GetValue()));
			}
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			log.InsertElement(duckdb::Hash<T>(values[selection.get_index(i)].GetValue()));
		}
	}
}

static bool TryUpdateSampleValues(HyperLogLog &log, const Vector &input, const SelectionVector &selection,
                                  idx_t count) {
	if (input.GetVectorType() != VectorType::FLAT_VECTOR) {
		return false;
	}
	switch (input.GetType().InternalType()) {
	case PhysicalType::INT8:
		UpdateSampleValues<int8_t>(log, input, selection, count);
		break;
	case PhysicalType::INT16:
		UpdateSampleValues<int16_t>(log, input, selection, count);
		break;
	case PhysicalType::INT32:
		UpdateSampleValues<int32_t>(log, input, selection, count);
		break;
	case PhysicalType::INT64:
		UpdateSampleValues<int64_t>(log, input, selection, count);
		break;
	case PhysicalType::UINT8:
		UpdateSampleValues<uint8_t>(log, input, selection, count);
		break;
	case PhysicalType::UINT16:
		UpdateSampleValues<uint16_t>(log, input, selection, count);
		break;
	case PhysicalType::UINT32:
		UpdateSampleValues<uint32_t>(log, input, selection, count);
		break;
	case PhysicalType::UINT64:
		UpdateSampleValues<uint64_t>(log, input, selection, count);
		break;
	case PhysicalType::INT128:
		UpdateSampleValues<hugeint_t>(log, input, selection, count);
		break;
	case PhysicalType::UINT128:
		UpdateSampleValues<uhugeint_t>(log, input, selection, count);
		break;
	case PhysicalType::FLOAT:
		UpdateSampleValues<float>(log, input, selection, count);
		break;
	case PhysicalType::DOUBLE:
		UpdateSampleValues<double>(log, input, selection, count);
		break;
	case PhysicalType::INTERVAL:
		UpdateSampleValues<interval_t>(log, input, selection, count);
		break;
	case PhysicalType::VARCHAR:
		UpdateSampleValues<string_t>(log, input, selection, count);
		break;
	default:
		return false;
	}
	return true;
}

void DistinctStatistics::Merge(const DistinctStatistics &other) {
	log->Merge(*other.log);
	sample_count += other.sample_count;
	total_count += other.total_count;
}

void DistinctStatistics::UpdateSample(const Vector &new_data, idx_t count, Vector &hashes) {
	total_count += count;
	const auto original_count = count;
	const auto sample_rate = new_data.GetType().IsIntegral() ? INTEGRAL_SAMPLE_RATE : BASE_SAMPLE_RATE;
	// Sample up to 'sample_rate' of STANDARD_VECTOR_SIZE of this vector (at least 1)
	count = MaxValue<idx_t>(LossyNumericCast<idx_t>(sample_rate * static_cast<double>(STANDARD_VECTOR_SIZE)), 1);
	// But never more than the original count
	count = MinValue<idx_t>(count, original_count);

	if (original_count != STANDARD_VECTOR_SIZE || count == original_count) {
		UpdateInternal(new_data, count, hashes);
		return;
	}

	static auto integral_sample =
	    CreateSampleSelection(MaxValue<idx_t>(LossyNumericCast<idx_t>(INTEGRAL_SAMPLE_RATE * STANDARD_VECTOR_SIZE), 1));
	static auto other_sample =
	    CreateSampleSelection(MaxValue<idx_t>(LossyNumericCast<idx_t>(BASE_SAMPLE_RATE * STANDARD_VECTOR_SIZE), 1));
	auto &sample_selection = new_data.GetType().IsIntegral() ? integral_sample : other_sample;
	if (TryUpdateSampleValues(*log, new_data, sample_selection, count)) {
		sample_count += count;
		return;
	}
	// Borrow the static indices to avoid reference-count contention between insertion threads.
	SelectionVector selection(sample_selection.data(), count);
	Vector sample(new_data, selection, count);
	UpdateInternal(sample, count, hashes);
}

void DistinctStatistics::Update(const Vector &new_data, idx_t count, Vector &hashes) {
	total_count += count;
	UpdateInternal(new_data, count, hashes);
}

void DistinctStatistics::UpdateInternal(const Vector &new_data, idx_t count, Vector &hashes) {
	sample_count += count;
	VectorOperations::Hash(new_data, hashes, count);

	log->Update(new_data, hashes);
}

string DistinctStatistics::ToString() const {
	return StringUtil::Format("[Approx Unique: %llu]", GetCount());
}

idx_t DistinctStatistics::GetCount() const {
	if (sample_count == 0 || total_count == 0) {
		return 0;
	}

	double u = static_cast<double>(MinValue<idx_t>(log->Count(), sample_count));
	double s = static_cast<double>(sample_count.load());
	double n = static_cast<double>(total_count.load());

	// Assume this proportion of the the sampled values occurred only once
	double u1 = pow(u / s, 2) * u;

	// Estimate total uniques using Good Turing Estimation
	idx_t estimate = LossyNumericCast<idx_t>(u + u1 / s * (n - s));
	return MinValue<idx_t>(estimate, total_count);
}

bool DistinctStatistics::TypeIsSupported(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::LIST:
	case PhysicalType::STRUCT:
	case PhysicalType::ARRAY:
		return false; // We don't support nested types
	case PhysicalType::BIT:
	case PhysicalType::BOOL:
		return false; // Doesn't make much sense
	default:
		return true;
	}
}

} // namespace duckdb
