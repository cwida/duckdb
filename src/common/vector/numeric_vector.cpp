#include "duckdb/common/vector/numeric_vector.hpp"

#include "duckdb/common/types/numeric_blob.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

bool NumericVector::TryGetTyped(const Vector &vector, unique_ptr<Vector> &typed) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::NUMERIC);
	if (vector.GetVectorType() == VectorType::SHREDDED_VECTOR) {
		auto &child = ShreddedVector::GetShreddedVector(vector);
		D_ASSERT(child.size() == vector.size());
		typed = make_uniq<Vector>(Vector::Ref(child));
		return true;
	}
	if (vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// a canonical constant: decode into the narrowest representation that holds it
		Vector constant(LogicalType::NUMERIC);
		constant.Reference(vector);
		auto type = NumericBlob::RangeType(constant, 1);
		typed = make_uniq<Vector>(type, idx_t(1));
		NumericBlob::Decode(constant, 1, *typed);
		typed->SetVectorType(VectorType::CONSTANT_VECTOR);
		return true;
	}
	return false;
}

//! The vector in another exact representation (every value fits by construction)
static unique_ptr<Vector> CastTyped(unique_ptr<Vector> source, const LogicalType &type) {
	if (source->GetType() == type) {
		return source;
	}
	if (source->GetVectorType() == VectorType::CONSTANT_VECTOR) {
		auto result = make_uniq<Vector>(type, idx_t(1));
		VectorOperations::DefaultCast(*source, *result, 1, true);
		result->SetVectorType(VectorType::CONSTANT_VECTOR);
		return result;
	}
	auto count = source->size();
	auto result = make_uniq<Vector>(type, count);
	VectorOperations::DefaultCast(*source, *result, count, true);
	return result;
}

bool NumericVector::TryNativePair(const Vector &left, const Vector &right, unique_ptr<Vector> &left_typed,
                                  unique_ptr<Vector> &right_typed) {
	if (!TryGetTyped(left, left_typed) || !TryGetTyped(right, right_typed)) {
		return false;
	}
	LogicalType common;
	if (!NumericType::TryWiden(left_typed->GetType(), right_typed->GetType(), common)) {
		// no representation holds both sides: they compare as canonical values
		return false;
	}
	left_typed = CastTyped(std::move(left_typed), common);
	right_typed = CastTyped(std::move(right_typed), common);
	// a constant carries the row count of its partner
	auto left_constant = left.GetVectorType() == VectorType::CONSTANT_VECTOR;
	auto right_constant = right.GetVectorType() == VectorType::CONSTANT_VECTOR;
	idx_t count;
	if (!left_constant) {
		count = left.size();
	} else if (!right_constant) {
		count = right.size();
	} else {
		count = MinValue(left.size(), right.size());
	}
	for (auto typed : {&left_typed, &right_typed}) {
		if ((*typed)->GetVectorType() == VectorType::CONSTANT_VECTOR && (*typed)->size() != count) {
			auto value = (*typed)->GetValue(0);
			*typed = make_uniq<Vector>(value, count_t(count));
		}
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Ranges
//===--------------------------------------------------------------------===//
template <class T>
static hugeint_t RangeValue(const T &value) {
	return Hugeint::Convert(value);
}

template <>
hugeint_t RangeValue(const hugeint_t &value) {
	return value;
}

template <class T>
static void ComputeRange(const Vector &typed, idx_t count, ShreddedNumericRange &range) {
	UnifiedVectorFormat vdata;
	typed.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	T min = T();
	T max = T();
	if (!vdata.sel->IsSet() && vdata.validity.CannotHaveNull()) {
		// flat without NULLs: a plain pass
		if (count > 0) {
			min = data[0];
			max = data[0];
		}
		for (idx_t i = 1; i < count; i++) {
			auto value = data[i];
			min = value < min ? value : min;
			max = value > max ? value : max;
		}
	} else {
		bool found = false;
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			if (!vdata.validity.RowIsValid(idx)) {
				continue;
			}
			auto value = data[idx];
			if (!found) {
				min = value;
				max = value;
				found = true;
			} else if (value < min) {
				min = value;
			} else if (value > max) {
				max = value;
			}
		}
	}
	range.min = RangeValue(min);
	range.max = RangeValue(max);
	range.valid = true;
}

bool NumericVector::TryComputeRange(const Vector &typed, idx_t count, ShreddedNumericRange &range) {
	auto &type = typed.GetType();
	range.scale = type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : 0;
	if (typed.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		count = 1;
	}
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		ComputeRange<int8_t>(typed, count, range);
		return true;
	case PhysicalType::INT16:
		ComputeRange<int16_t>(typed, count, range);
		return true;
	case PhysicalType::INT32:
		ComputeRange<int32_t>(typed, count, range);
		return true;
	case PhysicalType::INT64:
		ComputeRange<int64_t>(typed, count, range);
		return true;
	case PhysicalType::INT128:
		ComputeRange<hugeint_t>(typed, count, range);
		return true;
	case PhysicalType::UINT8:
		ComputeRange<uint8_t>(typed, count, range);
		return true;
	case PhysicalType::UINT16:
		ComputeRange<uint16_t>(typed, count, range);
		return true;
	case PhysicalType::UINT32:
		ComputeRange<uint32_t>(typed, count, range);
		return true;
	case PhysicalType::UINT64:
		ComputeRange<uint64_t>(typed, count, range);
		return true;
	default:
		range.valid = false;
		return false;
	}
}

bool NumericVector::TryGetRange(const Vector &vector, idx_t count, ShreddedNumericRange &range) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::NUMERIC);
	if (vector.GetVectorType() != VectorType::SHREDDED_VECTOR) {
		range.valid = false;
		return false;
	}
	auto &buffer = vector.GetBufferRef()->Cast<ShreddedVectorBuffer>();
	if (buffer.GetNumericRange().valid) {
		range = buffer.GetNumericRange();
		return true;
	}
	if (!TryComputeRange(buffer.GetChild(), count, range)) {
		return false;
	}
	buffer.SetNumericRange(range);
	return true;
}

void NumericVector::SetRange(Vector &vector, const ShreddedNumericRange &range) {
	D_ASSERT(vector.GetVectorType() == VectorType::SHREDDED_VECTOR);
	vector.GetBufferRef()->Cast<ShreddedVectorBuffer>().SetNumericRange(range);
}

} // namespace duckdb
