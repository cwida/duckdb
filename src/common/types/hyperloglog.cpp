#include "duckdb/common/types/hyperloglog.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "hyperloglog.hpp"

#include <math.h>

namespace duckdb_hll {
struct robj; // NOLINT
}

namespace duckdb {

//! Taken from redis code
double HyperLogLogBase::HLLSigma(double x) {
	if (x == 1.) {
		return std::numeric_limits<double>::infinity();
	}
	double z_prime;
	double y = 1;
	double z = x;
	do {
		x *= x;
		z_prime = z;
		z += x * y;
		y += y;
	} while (z_prime != z);
	return z;
}

//! Taken from redis code
double HyperLogLogBase::HLLTau(double x) {
	if (x == 0. || x == 1.) {
		return 0.;
	}
	double z_prime;
	double y = 1.0;
	double z = 1 - x;
	do {
		x = sqrt(x);
		z_prime = z;
		y *= 0.5;
		z -= pow(1 - x, 2) * y;
	} while (z_prime != z);
	return z / 3;
}

unique_ptr<HyperLogLog> HyperLogLog::Copy() const {
	auto result = make_uniq<HyperLogLog>();
	memcpy(result->k, this->k, sizeof(k));
	D_ASSERT(result->Count() == Count());
	return result;
}

class HLLV1 {
public:
	HLLV1() {
		hll = duckdb_hll::hll_create();
		duckdb_hll::hllSparseToDense(hll);
	}

	~HLLV1() {
		duckdb_hll::hll_destroy(hll);
	}

public:
	static idx_t GetSize() {
		return duckdb_hll::get_size();
	}

	data_ptr_t GetPtr() const {
		return data_ptr_cast((hll)->ptr);
	}

	void ToNew(HyperLogLog &new_hll) const {
		const idx_t mult = duckdb_hll::num_registers() / HyperLogLog::M;
		// Old implementation used more registers, so we compress the registers, losing some accuracy
		for (idx_t i = 0; i < HyperLogLog::M; i++) {
			uint8_t max_old = 0;
			for (idx_t j = 0; j < mult; j++) {
				D_ASSERT(i * mult + j < duckdb_hll::num_registers());
				max_old = MaxValue<uint8_t>(max_old, duckdb_hll::get_register(hll, i * mult + j));
			}
			new_hll.Update(i, max_old);
		}
	}

	void FromNew(const HyperLogLog &new_hll) {
		const auto new_hll_count = new_hll.Count();
		if (new_hll_count == 0) {
			return;
		}

		const idx_t mult = duckdb_hll::num_registers() / HyperLogLog::M;
		// When going from less to more registers, we cannot just duplicate the registers,
		// as each register in the new HLL is the minimum of 'mult' registers in the old HLL.
		// Duplicating will make for VERY large over-estimations. Instead, we do the following:

		// Set the first of every 'mult' registers in the old HLL to the value in the new HLL
		// This ensures that we can convert NEW to OLD and back to NEW without loss of information
		double avg = 0;
		for (idx_t i = 0; i < HyperLogLog::M; i++) {
			const auto max_new = MinValue(new_hll.GetRegister(i), duckdb_hll::maximum_zeros());
			duckdb_hll::set_register(hll, i * mult, max_new);
			avg += static_cast<double>(max_new);
		}
		avg /= static_cast<double>(HyperLogLog::M);

		// Using the average will ALWAYS overestimate, so we reduce it a bit here
		if (avg > 10) {
			avg *= 0.75;
		} else if (avg > 2) {
			avg -= 2;
		}

		// Set all other registers to a default value, starting with 0 (the initialization value)
		// We optimize the default value in 5 iterations or until OLD count is close to NEW count
		double default_val = 0;
		for (idx_t opt_idx = 0; opt_idx < 5; opt_idx++) {
			if (IsWithinAcceptableRange(new_hll_count, Count())) {
				break;
			}

			// Delta is half the average, then a quarter, etc.
			const double delta = avg / static_cast<double>(1 << (opt_idx + 1));
			if (Count() > new_hll_count) {
				default_val = delta > default_val ? 0 : default_val - delta;
			} else {
				default_val += delta;
			}

			// If the default value is, e.g., 3.3, then the first 70% gets value 3, and the rest gets value 4
			const double floor_fraction = 1 - (default_val - floor(default_val));
			for (idx_t i = 0; i < HyperLogLog::M; i++) {
				const auto max_new = MinValue(new_hll.GetRegister(i), duckdb_hll::maximum_zeros());
				uint8_t register_value;
				if (static_cast<double>(i) / static_cast<double>(HyperLogLog::M) < floor_fraction) {
					register_value = ExactNumericCast<uint8_t>(floor(default_val));
				} else {
					register_value = ExactNumericCast<uint8_t>(ceil(default_val));
				}
				register_value = MinValue(register_value, max_new);
				for (idx_t j = 1; j < mult; j++) {
					D_ASSERT(i * mult + j < duckdb_hll::num_registers());
					duckdb_hll::set_register(hll, i * mult + j, register_value);
				}
			}
		}
	}

private:
	idx_t Count() const {
		size_t result;
		if (duckdb_hll::hll_count(hll, &result) != HLL_C_OK) {
			throw InternalException("Could not count HLL?");
		}
		return result;
	}

	bool IsWithinAcceptableRange(const idx_t &new_hll_count, const idx_t &old_hll_count) const {
		const auto newd = static_cast<double>(new_hll_count);
		const auto oldd = static_cast<double>(old_hll_count);
		return MaxValue(newd, oldd) / MinValue(newd, oldd) < ACCEPTABLE_Q_ERROR;
	}

private:
	static constexpr double ACCEPTABLE_Q_ERROR = 1.2;
	duckdb_hll::robj *hll;
};

void HyperLogLog::Serialize(Serializer &serializer) const {
	if (serializer.ShouldSerialize(StorageVersion::V1_2_0)) {
		serializer.WriteProperty(100, "type", HLLStorageType::HLL_V2);
		serializer.WriteProperty(101, "data", k, sizeof(k));
	} else {
		auto old = make_uniq<HLLV1>();
		old->FromNew(*this);

		serializer.WriteProperty(100, "type", HLLStorageType::HLL_V1);
		serializer.WriteProperty(101, "data", old->GetPtr(), old->GetSize());
	}
}

unique_ptr<HyperLogLog> HyperLogLog::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<HyperLogLog>();
	auto storage_type = deserializer.ReadProperty<HLLStorageType>(100, "type");
	switch (storage_type) {
	case HLLStorageType::HLL_V1: {
		auto old = make_uniq<HLLV1>();
		deserializer.ReadProperty(101, "data", old->GetPtr(), old->GetSize());
		old->ToNew(*result);
		break;
	}
	case HLLStorageType::HLL_V2:
		deserializer.ReadProperty(101, "data", result->k, sizeof(k));
		break;
	default:
		throw SerializationException("Unknown HyperLogLog storage type!");
	}
	for (idx_t i = 0; i < M; i++) {
		// registers hold the number of leading zeros (plus one) of the hashes
		if (result->k[i] > Q + 1) {
			throw SerializationException("Invalid HyperLogLog register value %d", result->k[i]);
		}
	}
	return result;
}

} // namespace duckdb
