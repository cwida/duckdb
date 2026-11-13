#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/type_visitor.hpp"

#include "duckdb/function/cast/variant/to_variant.hpp"

namespace duckdb {
namespace variant {

void ThrowEnumIndexOutOfRange(idx_t index, idx_t size) {
	throw InternalException("ENUM index %llu is out of range for an ENUM with %llu values", index, size);
}

void InitializeOffsets(DataChunk &offsets, idx_t count) {
	auto keys = OffsetData::GetKeys(offsets);
	auto children = OffsetData::GetChildren(offsets);
	auto values = OffsetData::GetValues(offsets);
	auto blob = OffsetData::GetBlob(offsets);
	for (idx_t i = 0; i < count; i++) {
		keys[i] = 0;
		children[i] = 0;
		values[i] = 0;
		blob[i] = 0;
	}
}

static void InitializeVariants(DataChunk &offsets, Vector &result, SelectionVector &keys_selvec, idx_t &selvec_size,
                               OrderedOwningStringMap<uint32_t> &dictionary) {
	auto &keys = VariantVector::GetKeys(result);
	auto keys_data = FlatVector::GetDataMutable<list_entry_t>(keys);

	auto &children = VariantVector::GetChildren(result);
	auto children_data = FlatVector::GetDataMutable<list_entry_t>(children);

	auto &values = VariantVector::GetValues(result);
	auto values_data = FlatVector::GetDataMutable<list_entry_t>(values);

	auto &blob = VariantVector::GetData(result);
	auto blob_data = FlatVector::GetDataMutable<string_t>(blob);

	idx_t keys_offset = 0;
	idx_t children_offset = 0;
	idx_t values_offset = 0;

	auto keys_sizes = OffsetData::GetKeys(offsets);
	auto children_sizes = OffsetData::GetChildren(offsets);
	auto values_sizes = OffsetData::GetValues(offsets);
	auto blob_sizes = OffsetData::GetBlob(offsets);
	for (idx_t i = 0; i < offsets.size(); i++) {
		auto &keys_entry = keys_data[i];
		auto &children_entry = children_data[i];
		auto &values_entry = values_data[i];

		//! keys
		keys_entry.length = keys_sizes[i];
		keys_entry.offset = keys_offset;
		keys_offset += keys_entry.length;

		//! children
		children_entry.length = children_sizes[i];
		children_entry.offset = children_offset;
		children_offset += children_entry.length;

		//! values
		values_entry.length = values_sizes[i];
		values_entry.offset = values_offset;
		values_offset += values_entry.length;

		//! value
		blob_data[i] = StringVector::EmptyString(blob, blob_sizes[i]);
	}

	//! Reserve for the children of the lists
	ListVector::Reserve(keys, keys_offset);
	ListVector::Reserve(children, children_offset);
	ListVector::Reserve(values, values_offset);

	//! Set list sizes
	ListVector::SetListSize(keys, keys_offset);
	ListVector::SetListSize(children, children_offset);
	ListVector::SetListSize(values, values_offset);

	keys_selvec.Initialize(keys_offset);
	selvec_size = keys_offset;
}

static bool GatherOffsetsAndSizes(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count) {
	//! First pass - collect sizes/offsets
	return ConvertToVariant<false>(source, result, count, nullptr, nullptr, true);
}

static bool WriteVariantResultData(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count) {
	//! Second pass - actually construct the variants
	return ConvertToVariant<true>(source, result, count, nullptr, nullptr, true);
}

//! Builds a VARIANT vector in two passes: 'prepare' collects the sizes/offsets that the result needs, and 'write'
//! then constructs the variants themselves
template <class PREPARE, class WRITE>
static bool BuildVariant(Vector &result, idx_t count, Allocator &allocator, PREPARE prepare, WRITE write) {
	DataChunk offsets;
	offsets.Initialize(Allocator::DefaultAllocator(),
	                   {LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::UINTEGER},
	                   count);
	offsets.SetChildCardinality(count);
	auto &keys_entry = ListVector::GetChildMutable(VariantVector::GetKeys(result));

	//! Initialize the dictionary
	OrderedOwningStringMap<uint32_t> dictionary(StringVector::GetStringAllocator(keys_entry));
	SelectionVector keys_selvec;

	{
		VariantVectorData variant_data(result);
		ToVariantGlobalResultData result_data(variant_data, offsets, dictionary, keys_selvec, allocator);
		InitializeOffsets(offsets, count);
		if (!prepare(result_data, offsets)) {
			return false;
		}
	}

	//! This resizes the lists, invalidating the "GetData" results stored in VariantVectorData
	idx_t keys_selvec_size;
	InitializeVariants(offsets, result, keys_selvec, keys_selvec_size, dictionary);

	{
		VariantVectorData variant_data(result);
		ToVariantGlobalResultData result_data(variant_data, offsets, dictionary, keys_selvec, allocator);
		InitializeOffsets(offsets, count);
		if (!write(result_data, offsets)) {
			return false;
		}
	}

	VariantUtils::FinalizeVariantKeys(result, dictionary, keys_selvec, keys_selvec_size);
	//! Finalize the 'data'
	auto blob_data = FlatVector::GetDataMutable<string_t>(VariantVector::GetData(result));
	auto blob_offsets = OffsetData::GetBlob(offsets);
	for (idx_t i = 0; i < count; i++) {
		auto size = blob_offsets[i];
		blob_data[i].SetSizeAndFinalize(size, size);
	}

	keys_entry.Slice(keys_selvec, keys_selvec_size);
	result.Verify();
	return true;
}

bool ConvertJSONValuesToVariant(yyjson_val *values[], idx_t count, Vector &result) {
	if (!count) {
		return true;
	}
	D_ASSERT(result.GetType().id() == LogicalTypeId::VARIANT);

	//! First pass - collect sizes/offsets
	auto prepare = [&](ToVariantGlobalResultData &result_data, DataChunk &offsets) {
		vector<JSONConversionFrame> stack;
		for (idx_t i = 0; i < count; i++) {
			if (!values[i]) {
				//! No value - the row stays empty and is set to NULL in the second pass
				continue;
			}
			if (!ConvertJSON<false, false>(values[i], result_data, i, true, stack)) {
				return false;
			}
		}
		return true;
	};

	//! Second pass - actually construct the variants
	auto write = [&](ToVariantGlobalResultData &result_data, DataChunk &offsets) {
		auto values_offsets = OffsetData::GetValues(offsets);
		auto blob_offsets = OffsetData::GetBlob(offsets);
		vector<JSONConversionFrame> stack;
		for (idx_t i = 0; i < count; i++) {
			if (!values[i]) {
				HandleVariantNull<true>(result_data, i, values_offsets, blob_offsets[i], nullptr, 0, true);
				continue;
			}
			if (!ConvertJSON<true, false>(values[i], result_data, i, true, stack)) {
				return false;
			}
		}
		return true;
	};

	return BuildVariant(result, count, Allocator::DefaultAllocator(), prepare, write);
}

struct VariantCastData : BoundCastData {
	explicit VariantCastData(const LogicalType &source_type_p)
	    : shredded_type(VariantUtils::ShreddedType(source_type_p)) {
	}

	LogicalType shredded_type;

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<VariantCastData>(*this);
	}
};

struct VariantLocalData : FunctionLocalState {
	VariantLocalData(const LogicalType &shredded_type, Allocator &allocator) : allocator(allocator) {
		Initialize(shredded_type, STANDARD_VECTOR_SIZE);
	}

	//! Allocator for temporary conversion memory (e.g. JSON parsing), backed by the buffer allocator when possible
	//! so the memory is tracked
	Allocator &allocator;

	void Initialize(const LogicalType &shredded_type, idx_t new_capacity) {
		capacity = new_capacity;
		shredded_vector = make_uniq<Vector>(shredded_type, capacity);
		auto &top_shredded = StructVector::GetEntries(*shredded_vector);
		// NULL out everything in the unshredded part
		auto &unshredded_child = top_shredded[0];
		for (auto &unshredded_entry : StructVector::GetEntries(unshredded_child)) {
			ConstantVector::SetNull(unshredded_entry, count_t(new_capacity));
		}
		ConstantVector::SetNull(unshredded_child, count_t(new_capacity));
	}

	Vector &GetShreddedVector(idx_t req_capacity) {
		if (req_capacity <= capacity) {
			return *shredded_vector;
		}
		Initialize(shredded_vector->GetType(), req_capacity);
		return *shredded_vector;
	}

private:
	idx_t capacity;
	unique_ptr<Vector> shredded_vector;
};

unique_ptr<FunctionLocalState> CastToVariantLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<VariantCastData>();
	auto &allocator = parameters.context ? BufferAllocator::Get(*parameters.context) : Allocator::DefaultAllocator();
	return make_uniq<VariantLocalData>(cast_data.shredded_type, allocator);
}

static bool SupportsShreddedCast(const LogicalType &type) {
	if (type.id() == LogicalTypeId::STRUCT) {
		// for struct types recurse into the child types
		auto &child_types = StructType::GetChildTypes(type);
		if (child_types.empty()) {
			// an empty struct has no typed_value to shred into
			return false;
		}
		for (auto &entry : child_types) {
			if (!SupportsShreddedCast(entry.second)) {
				return false;
			}
		}
		return true;
	}
	if (!VariantUtils::VariantSupportsType(type)) {
		// type is not natively supported in variant so it cannot be emitted as a shredded type without conversion
		return false;
	}
	return true;
}

//! Whether the storage can shred 'type', it recurses into objects and arrays
static bool SupportsShreddedType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		auto &child_types = StructType::GetChildTypes(type);
		if (child_types.empty()) {
			// an empty object has no typed value to shred into
			return false;
		}
		for (auto &child : child_types) {
			if (!SupportsShreddedType(child.second)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return SupportsShreddedType(ListType::GetChildType(type));
	default:
		return VariantUtils::VariantSupportsType(type);
	}
}

//! The type that 'type' is shredded into. Every value keeps an 'untyped_value_index', so that values that do not
//! match the shredded type can be stored in the unshredded part of the VARIANT.
static LogicalType ShreddedTypeWithFallback(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> children;
		for (auto &child : StructType::GetChildTypes(type)) {
			children.emplace_back(child.first, ShreddedTypeWithFallback(child.second));
		}
		auto typed_value = LogicalType::STRUCT(std::move(children));
		return LogicalType::STRUCT(
		    {{"typed_value", std::move(typed_value)}, {"untyped_value_index", LogicalType::UINTEGER}});
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto typed_value = LogicalType::LIST(ShreddedTypeWithFallback(ListType::GetChildType(type)));
		return LogicalType::STRUCT(
		    {{"typed_value", std::move(typed_value)}, {"untyped_value_index", LogicalType::UINTEGER}});
	}
	default:
		return LogicalType::STRUCT({{"typed_value", type}, {"untyped_value_index", LogicalType::UINTEGER}});
	}
}

static void ShreddedVectorReference(const Vector &source, Vector &result, idx_t count) {
	if (source.GetType().id() == LogicalTypeId::STRUCT) {
		// source is "{<children>}", target is "{typed value STRUCT(<children>)}"
		// go into the "typed_value"
		auto &typed_value = StructVector::GetEntries(result)[0];
		// copy over the validity
		FlatVector::CopyValidity(result, source, count);
		FlatVector::CopyValidity(typed_value, source, count);
		// now recurse into the children of both
		auto &source_entries = StructVector::GetEntries(source);
		auto &target_entries = StructVector::GetEntries(typed_value);
		for (idx_t child_idx = 0; child_idx < source_entries.size(); child_idx++) {
			ShreddedVectorReference(source_entries[child_idx], target_entries[child_idx], count);
		}
		return;
	}
	// for primitive types we can directly reference the input
	result.Reference(source);
}

static bool TryToShreddedCast(Vector &source, Vector &result, idx_t count, VariantLocalData &local_data) {
	if (!SupportsShreddedCast(source.GetType())) {
		return false;
	}
	auto &shredded_vector = local_data.GetShreddedVector(count);
	// emit a shredded vector that references the source directly
	auto &top_shredded = StructVector::GetEntries(shredded_vector);
	auto &shredded_child = top_shredded[1];
	ShreddedVectorReference(source, shredded_child, count);
	FlatVector::SetSize(shredded_vector, count_t(count));
	result.Shred(shredded_vector, count);
	return true;
}

static bool ConvertUnshredded(Vector &source, Vector &result, idx_t count, Allocator &allocator) {
	ToVariantSourceData source_data(source, count);

	//! First pass - collect sizes/offsets
	auto prepare = [&](ToVariantGlobalResultData &result_data, DataChunk &offsets) {
		return GatherOffsetsAndSizes(source_data, result_data, count);
	};

	//! Second pass - actually construct the variants
	auto write = [&](ToVariantGlobalResultData &result_data, DataChunk &offsets) {
		return WriteVariantResultData(source_data, result_data, count);
	};

	return BuildVariant(result, count, allocator, prepare, write);
}

VariantShredder::VariantShredder(const LogicalType &source_type)
    : supports_shredding(SupportsShreddedType(source_type)) {
	if (supports_shredding) {
		shredded_type = LogicalType::STRUCT({{"unshredded", VariantShredding::GetUnshreddedType()},
		                                     {"shredded", ShreddedTypeWithFallback(source_type)}});
	}
}

VariantShredder::~VariantShredder() {
}

bool VariantShredder::Shred(Vector &unshredded, Vector &result, idx_t count) {
	if (!count) {
		return true;
	}
	D_ASSERT(result.GetType().id() == LogicalTypeId::VARIANT);
	D_ASSERT(supports_shredding);
	FlatVector::SetSize(unshredded, count_t(count));
	//! A fresh vector per call: writing the shredded data slices the keys of the unshredded part
	Vector shredded(shredded_type, count);
	// values that do not conform to the shredded type are moved to the unshredded part of the variant
	VariantShredding::ShredWithFallback(unshredded, shredded, count);
	FlatVector::SetSize(shredded, count_t(count));
	result.Shred(shredded, count);
	return true;
}

static bool CastToVARIANT(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (!count) {
		return true;
	}
	auto &local_data = parameters.local_state->Cast<VariantLocalData>();
	if (TryToShreddedCast(source, result, count, local_data)) {
		return true;
	}
	return ConvertUnshredded(source, result, count, local_data.allocator);
}

} // namespace variant

BoundCastInfo DefaultCasts::ImplicitToVariantCast(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	if (TypeVisitor::Contains(source, LogicalTypeId::TYPE)) {
		//! TYPE has no VARIANT representation - only NULL values can be cast
		return TryVectorNullCast;
	}
	return BoundCastInfo(variant::CastToVARIANT, make_uniq<variant::VariantCastData>(source),
	                     variant::CastToVariantLocalState);
}

} // namespace duckdb
