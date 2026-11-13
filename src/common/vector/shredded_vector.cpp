#include "duckdb/common/vector/shredded_vector.hpp"
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

void ShreddedVectorBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(type.id() == LogicalTypeId::VARIANT);
	D_ASSERT(vector_type == VectorType::SHREDDED_VECTOR);
	shredded_data->Verify(sel, count);
	D_ASSERT(shredded_data->size() == Size());
}

string ShreddedVectorBuffer::ToString(const LogicalType &type, idx_t count) const {
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
	if (type.id() != LogicalTypeId::VARIANT) {
		throw SerializationException("Shredded vectors can only be deserialized as VARIANT vectors");
	}
	auto shredded_type = deserializer.ReadProperty<LogicalType>(91, "shredded_type");
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
	Vector shredded_data(shredded_type, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	deserializer.ReadObject(92, "shredded_data", [&](Deserializer &obj) { shredded_data.Deserialize(obj, count); });
	FlatVector::SetSize(shredded_data, count);
	return make_buffer<ShreddedVectorBuffer>(shredded_data, count_t(count));
}

Value ShreddedVectorBuffer::GetValue(const LogicalType &type, idx_t index) const {
	// FIXME: this is extremely inefficient
	auto &shredded = StructVector::GetEntries(*shredded_data)[1];
	auto &unshredded = StructVector::GetEntries(*shredded_data)[0];

	auto shredded_val = shredded.GetValue(index);
	auto unshredded_val = unshredded.GetValue(index);

	child_list_t<LogicalType> shredded_subtypes;
	shredded_subtypes.emplace_back(make_pair("unshredded", unshredded.GetType()));
	shredded_subtypes.emplace_back(make_pair("shredded", shredded.GetType()));
	Vector new_shredded(LogicalType::STRUCT(std::move(shredded_subtypes)));
	StructVector::GetEntries(new_shredded)[0].Reference(unshredded_val, count_t(1));
	StructVector::GetEntries(new_shredded)[1].Reference(shredded_val, count_t(1));

	Vector result_vec(LogicalType::VARIANT(), 1);
	VariantUtils::UnshredVariantData(new_shredded, result_vec, 1);
	return result_vec.GetValue(0);
}

buffer_ptr<VectorBuffer> ShreddedVectorBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	// propagate the slice into the shredded data and emit a new shredded vector
	auto count = count_t(end - offset);
	Vector sliced(*shredded_data, offset, end);
	return make_buffer<ShreddedVectorBuffer>(sliced, count);
}

buffer_ptr<VectorBuffer> ShreddedVectorBuffer::SliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                             idx_t count) {
	// propagate the slice into the shredded data and emit a new shredded vector
	Vector sliced(*shredded_data, sel, count);
	return make_buffer<ShreddedVectorBuffer>(sliced, count_t(count));
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
	return StructVector::GetEntries(vec.GetBufferRef()->Cast<ShreddedVectorBuffer>().GetChild())[0];
}

Vector &ShreddedVector::GetUnshreddedVector(Vector &vec) {
	VerifyShreddedVector(vec);
	return StructVector::GetEntries(vec.BufferMutable().Cast<ShreddedVectorBuffer>().GetChild())[0];
}

const Vector &ShreddedVector::GetShreddedVector(const Vector &vec) {
	VerifyShreddedVector(vec);
	return StructVector::GetEntries(vec.GetBufferRef()->Cast<ShreddedVectorBuffer>().GetChild())[1];
}

Vector &ShreddedVector::GetShreddedVector(Vector &vec) {
	VerifyShreddedVector(vec);
	return StructVector::GetEntries(vec.BufferMutable().Cast<ShreddedVectorBuffer>().GetChild())[1];
}

void ShreddedVector::Unshred(const Vector &vec, idx_t count) {
	Vector unshredded_vector(LogicalType::VARIANT(), MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	auto &shredded_buffer = vec.GetBufferRef()->Cast<ShreddedVectorBuffer>();
	VariantUtils::UnshredVariantData(shredded_buffer.GetChild(), unshredded_vector, count);
	vec.ConstReference(unshredded_vector);
}

void ShreddedVector::Unshred(const Vector &vec, const SelectionVector &sel, idx_t count) {
	VerifyShreddedVector(vec);
	// slice the underlying shredded buffer
	auto &shredded_buffer = vec.GetBufferRef()->Cast<ShreddedVectorBuffer>();
	Vector sliced_shredded_buffer(shredded_buffer.GetChild(), sel, count);
	// unshred the vector
	Vector unshredded_vector(LogicalType::VARIANT());
	VariantUtils::UnshredVariantData(sliced_shredded_buffer, unshredded_vector, count);
	vec.ConstReference(unshredded_vector);
}

bool ShreddedVector::IsFullyShredded(const Vector &vec) {
	auto &unshredded_vector = GetUnshreddedVector(vec);
	if (unshredded_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && ConstantVector::IsNull(unshredded_vector)) {
		return true;
	}
	return false;
}

optional_ptr<const ValidityMask> ShreddedVector::TryGetRowValidity(const Vector &vec) {
	VerifyShreddedVector(vec);
	// the top-level validity of the shredded component holds the per-row validity of the VARIANT
	auto &shredded = GetShreddedVector(vec);
	if (shredded.GetVectorType() != VectorType::FLAT_VECTOR) {
		return nullptr;
	}
	return FlatVector::Validity(shredded);
}

} // namespace duckdb
