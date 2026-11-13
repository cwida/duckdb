#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/types/numeric_blob.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"

namespace duckdb {

ShreddedVectorBuffer::ShreddedVectorBuffer(Vector &shredded_data_p, count_t count_p)
    : VectorBuffer(VectorType::SHREDDED_VECTOR, VectorBufferType::SHREDDED_BUFFER, count_p),
      shredded_data(make_uniq<Vector>(Vector::Ref(shredded_data_p))) {
}

ShreddedVectorBuffer::~ShreddedVectorBuffer() {
}

idx_t ShreddedVectorBuffer::GetDataSize(const LogicalType &type, idx_t count) const {
	return shredded_data->GetDataSize(count);
}

idx_t ShreddedVectorBuffer::GetAllocationSize() const {
	idx_t size = VectorBuffer::GetAllocationSize();
	size += shredded_data->GetAllocationSize();
	return size;
}

static bool IsNumericType(const LogicalType &type) {
	return type.id() == LogicalTypeId::NUMERIC;
}

void ShreddedVectorBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(type.id() == LogicalTypeId::VARIANT || type.id() == LogicalTypeId::NUMERIC);
	D_ASSERT(vector_type == VectorType::SHREDDED_VECTOR);
	shredded_data->Verify(sel, count);
	D_ASSERT(shredded_data->size() == Size());
}

string ShreddedVectorBuffer::ToString(const LogicalType &type, idx_t count) const {
	if (IsNumericType(type)) {
		return "NUMERIC as " + shredded_data->GetType().ToString() + ": " + shredded_data->ToString();
	}
	auto &shredded = StructVector::GetEntries(*shredded_data)[1];
	auto &unshredded = StructVector::GetEntries(*shredded_data)[0];
	return "Shredded: " + shredded.ToString() + ", Unshredded: " + unshredded.ToString();
}

void ShreddedVectorBuffer::SetVectorType(VectorType new_vector_type) {
	throw InternalException("ShreddedVectorBuffer::SetVectorType is not implemented and shouldn't be reached");
}

bool ShreddedVectorBuffer::TrySerialize(Serializer &serializer, const LogicalType &type,
                                        bool compressed_serialization) const {
	// older versions cannot read shredded vectors - these are unshredded prior to serialization instead
	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		return false;
	}
	serializer.WriteProperty(90, "vector_type", VectorType::SHREDDED_VECTOR);
	serializer.WriteProperty(91, "shredded_type", shredded_data->GetType());
	serializer.WriteObject(92, "shredded_data", [&](Serializer &object) {
		auto serialized_vector = Vector::Ref(*shredded_data);
		serialized_vector.Serialize(object, compressed_serialization);
	});
	return true;
}

buffer_ptr<VectorBuffer> ShreddedVectorBuffer::Deserialize(Deserializer &deserializer, const LogicalType &type,
                                                           idx_t count) {
	if (type.id() != LogicalTypeId::VARIANT && type.id() != LogicalTypeId::NUMERIC) {
		throw SerializationException("Shredded vectors can only be deserialized as VARIANT or NUMERIC vectors");
	}
	auto shredded_type = deserializer.ReadProperty<LogicalType>(91, "shredded_type");
	if (IsNumericType(type)) {
		// the child of a NUMERIC vector is one exact representation, not the VARIANT struct
		if (!NumericType::IsExact(shredded_type)) {
			throw SerializationException("The representation of a NUMERIC vector must be an exact numeric type");
		}
	} else {
		if (shredded_type.id() != LogicalTypeId::STRUCT || StructType::GetChildCount(shredded_type) != 2) {
			throw SerializationException("Shredded vector data must be a struct with two children");
		}
		auto &unshredded_type = StructType::GetChildType(shredded_type, 0);
		if (unshredded_type != VariantShredding::GetUnshreddedType()) {
			throw SerializationException("Shredded vector data's first child must be the canonical VARIANT struct");
		}
		auto &inner_shredded_type = StructType::GetChildType(shredded_type, 1);
		if (TypeVisitor::Contains(inner_shredded_type, LogicalTypeId::VARIANT)) {
			throw SerializationException("Shredded vector data's second child cannot itself contain a VARIANT type");
		}
	}
	Vector shredded_data(shredded_type, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	deserializer.ReadObject(92, "shredded_data", [&](Deserializer &obj) { shredded_data.Deserialize(obj, count); });
	FlatVector::SetSize(shredded_data, count);
	return make_buffer<ShreddedVectorBuffer>(shredded_data, count_t(count));
}

Value ShreddedVectorBuffer::GetValue(const LogicalType &type, idx_t index) const {
	if (IsNumericType(type)) {
		auto typed_value = shredded_data->GetValue(index);
		if (typed_value.IsNull()) {
			return Value(LogicalType::NUMERIC);
		}
		return Value::NUMERIC(typed_value.ToString());
	}
	Vector input(*shredded_data, index, index + 1);
	Vector result(LogicalType::VARIANT(), 1);
	VariantUtils::UnshredVariantData(input, result, 1);
	return result.GetValue(0);
}

buffer_ptr<VectorBuffer> ShreddedVectorBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	// propagate the slice into the shredded data and emit a new shredded vector
	auto count = count_t(end - offset);
	Vector sliced(*shredded_data, offset, end);
	auto result = make_buffer<ShreddedVectorBuffer>(sliced, count);
	result->numeric_range = numeric_range;
	return result;
}

buffer_ptr<VectorBuffer> ShreddedVectorBuffer::SliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                             idx_t count) {
	// propagate the slice into the shredded data and emit a new shredded vector
	Vector sliced(*shredded_data, sel, count);
	auto result = make_buffer<ShreddedVectorBuffer>(sliced, count_t(count));
	result->numeric_range = numeric_range;
	return result;
}

buffer_ptr<VectorBuffer> ShreddedVectorBuffer::FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                                    idx_t count) const {
	Vector *source = shredded_data.get();
	// if a selection vector is provided, slice the shredded data first
	unique_ptr<Vector> sliced;
	if (sel.IsSet()) {
		sliced = make_uniq<Vector>(*shredded_data, sel, count);
		source = sliced.get();
	}
	if (IsNumericType(type)) {
		// the canonical representation of the values
		Vector canonical(LogicalType::NUMERIC, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
		NumericBlob::Encode(*source, count, canonical);
		auto result = canonical.GetBufferRef();
		result->SetVectorSize(count);
		return result;
	}
	// unshred the (optionally sliced) vector
	Vector unshredded_vector(LogicalType::VARIANT(), MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	FlatVector::SetSize(unshredded_vector, count);
	VariantUtils::UnshredVariantData(*source, unshredded_vector, count);
	// now flatten the unshredded vector
	unshredded_vector.Flatten();
	auto result = unshredded_vector.GetBufferRef();
	result->SetVectorSize(count);
	return result;
}

const Vector &ShreddedVector::GetUnshreddedVector(const Vector &vec) {
	VerifyShreddedVector(vec);
	if (IsNumericType(vec.GetType())) {
		throw InternalException("A NUMERIC vector has no unshredded part");
	}
	return StructVector::GetEntries(vec.GetBufferRef()->Cast<ShreddedVectorBuffer>().GetChild())[0];
}

Vector &ShreddedVector::GetUnshreddedVector(Vector &vec) {
	VerifyShreddedVector(vec);
	if (IsNumericType(vec.GetType())) {
		throw InternalException("A NUMERIC vector has no unshredded part");
	}
	return StructVector::GetEntries(vec.BufferMutable().Cast<ShreddedVectorBuffer>().GetChild())[0];
}

const Vector &ShreddedVector::GetShreddedVector(const Vector &vec) {
	VerifyShreddedVector(vec);
	auto &child = vec.GetBufferRef()->Cast<ShreddedVectorBuffer>().GetChild();
	if (IsNumericType(vec.GetType())) {
		// the representation of a NUMERIC vector is the child itself
		return child;
	}
	return StructVector::GetEntries(child)[1];
}

Vector &ShreddedVector::GetShreddedVector(Vector &vec) {
	VerifyShreddedVector(vec);
	auto &child = vec.BufferMutable().Cast<ShreddedVectorBuffer>().GetChild();
	if (IsNumericType(vec.GetType())) {
		return child;
	}
	return StructVector::GetEntries(child)[1];
}

static void UnshredNumeric(const Vector &vec, Vector &child, idx_t count) {
	Vector canonical(LogicalType::NUMERIC, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	NumericBlob::Encode(child, count, canonical);
	vec.ConstReference(canonical);
}

void ShreddedVector::Unshred(const Vector &vec, idx_t count) {
	auto &shredded_buffer = vec.GetBufferRef()->Cast<ShreddedVectorBuffer>();
	if (IsNumericType(vec.GetType())) {
		UnshredNumeric(vec, shredded_buffer.GetChild(), count);
		return;
	}
	Vector unshredded_vector(LogicalType::VARIANT(), MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	VariantUtils::UnshredVariantData(shredded_buffer.GetChild(), unshredded_vector, count);
	vec.ConstReference(unshredded_vector);
}

void ShreddedVector::Unshred(const Vector &vec, const SelectionVector &sel, idx_t count) {
	VerifyShreddedVector(vec);
	// slice the underlying shredded buffer
	auto &shredded_buffer = vec.GetBufferRef()->Cast<ShreddedVectorBuffer>();
	Vector sliced_shredded_buffer(shredded_buffer.GetChild(), sel, count);
	if (IsNumericType(vec.GetType())) {
		UnshredNumeric(vec, sliced_shredded_buffer, count);
		return;
	}
	// unshred the vector
	Vector unshredded_vector(LogicalType::VARIANT());
	VariantUtils::UnshredVariantData(sliced_shredded_buffer, unshredded_vector, count);
	vec.ConstReference(unshredded_vector);
}

bool ShreddedVector::IsFullyShredded(const Vector &vec) {
	if (IsNumericType(vec.GetType())) {
		return true;
	}
	auto &unshredded_vector = GetUnshreddedVector(vec);
	if (unshredded_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && ConstantVector::IsNull(unshredded_vector)) {
		return true;
	}
	return false;
}

} // namespace duckdb
