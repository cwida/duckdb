#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

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
	InitializeOffsets(result.offsets, count);
	//! First pass - collect sizes/offsets
	return ConvertToVariant<false>(source, result, count, nullptr, nullptr, true);
}

static bool WriteVariantResultData(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count) {
	InitializeOffsets(result.offsets, count);
	//! Second pass - actually construct the variants
	return ConvertToVariant<true>(source, result, count, nullptr, nullptr, true);
}

struct VariantCastData : BoundCastData {
	VariantCastData(const LogicalType &variant_source_type, unique_ptr<BoundCastInfo> pre_cast_p)
	    : shredded_type(VariantUtils::ShreddedType(variant_source_type)), pre_cast_target(variant_source_type),
	      pre_cast(std::move(pre_cast_p)) {
	}

	LogicalType shredded_type;
	LogicalType pre_cast_target;
	//! Casts custom types in the source to their registered JSON representation before converting to VARIANT
	unique_ptr<BoundCastInfo> pre_cast;

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<VariantCastData>(pre_cast_target,
		                                  pre_cast ? make_uniq<BoundCastInfo>(pre_cast->Copy()) : nullptr);
	}
};

struct VariantLocalData : FunctionLocalState {
	VariantLocalData(const LogicalType &shredded_type, Allocator &allocator) : allocator(allocator) {
		Initialize(shredded_type, STANDARD_VECTOR_SIZE);
	}

	//! Allocator for temporary conversion memory (e.g. JSON parsing), backed by the buffer allocator when possible
	//! so the memory is tracked
	Allocator &allocator;

	//! Local state of the cast that converts custom types to their JSON representation
	unique_ptr<FunctionLocalState> pre_cast_local_state;

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
	auto result = make_uniq<VariantLocalData>(cast_data.shredded_type, allocator);
	if (cast_data.pre_cast && cast_data.pre_cast->HasInitLocalState()) {
		CastLocalStateParameters pre_cast_params(parameters, cast_data.pre_cast->GetCastData());
		result->pre_cast_local_state = cast_data.pre_cast->InitLocalState(pre_cast_params);
	}
	return std::move(result);
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

static bool TryToShreddedCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (!SupportsShreddedCast(source.GetType())) {
		return false;
	}
	auto &local_data = parameters.local_state->Cast<VariantLocalData>();
	auto &shredded_vector = local_data.GetShreddedVector(count);
	// emit a shredded vector that references the source directly
	auto &top_shredded = StructVector::GetEntries(shredded_vector);
	auto &shredded_child = top_shredded[1];
	ShreddedVectorReference(source, shredded_child, count);
	FlatVector::SetSize(shredded_vector, count_t(count));
	result.Shred(shredded_vector, count);
	return true;
}

static bool CastToVARIANT(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (!count) {
		return true;
	}
	auto &local_data = parameters.local_state->Cast<VariantLocalData>();
	auto &cast_data = parameters.cast_data->Cast<VariantCastData>();

	optional_ptr<Vector> source_vector(&source);
	unique_ptr<Vector> pre_cast_vector;
	if (cast_data.pre_cast) {
		pre_cast_vector = make_uniq<Vector>(cast_data.pre_cast_target, count);
		CastParameters pre_cast_params(parameters, cast_data.pre_cast->GetCastData(), local_data.pre_cast_local_state);
		if (!cast_data.pre_cast->Cast(source, *pre_cast_vector, count, pre_cast_params)) {
			return false;
		}
		source_vector = pre_cast_vector.get();
	}
	auto &converted_source = *source_vector;

	if (TryToShreddedCast(converted_source, result, count, parameters)) {
		return true;
	}
	DataChunk offsets;
	offsets.Initialize(Allocator::DefaultAllocator(),
	                   {LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::UINTEGER},
	                   count);
	offsets.SetChildCardinality(count);
	auto &keys = VariantVector::GetKeys(result);
	auto &keys_entry = ListVector::GetChildMutable(keys);

	//! Initialize the dictionary
	OrderedOwningStringMap<uint32_t> dictionary(StringVector::GetStringAllocator(keys_entry));
	SelectionVector keys_selvec;
	ToVariantSourceData source_data(converted_source, count);

	{
		VariantVectorData variant_data(result);
		ToVariantGlobalResultData result_data(variant_data, offsets, dictionary, keys_selvec, local_data.allocator);
		if (!GatherOffsetsAndSizes(source_data, result_data, count)) {
			return false;
		}
	}

	//! This resizes the lists, invalidating the "GetData" results stored in VariantVectorData
	idx_t keys_selvec_size;
	InitializeVariants(offsets, result, keys_selvec, keys_selvec_size, dictionary);

	{
		VariantVectorData variant_data(result);
		ToVariantGlobalResultData result_data(variant_data, offsets, dictionary, keys_selvec, local_data.allocator);
		if (!WriteVariantResultData(source_data, result_data, count)) {
			return false;
		}
	}

	VariantUtils::FinalizeVariantKeys(result, dictionary, keys_selvec, keys_selvec_size);
	//! Finalize the 'data'
	auto &blob = VariantVector::GetData(result);
	auto blob_data = FlatVector::GetDataMutable<string_t>(blob);
	auto blob_offsets = OffsetData::GetBlob(offsets);
	for (idx_t i = 0; i < count; i++) {
		auto size = blob_offsets[i];
		blob_data[i].SetSizeAndFinalize(size, size);
	}

	keys_entry.Slice(keys_selvec, keys_selvec_size);
	result.Verify();
	return true;
}

//! The type that is converted to VARIANT: custom types with a registered JSON cast become JSON, so the
//! conversion stores the value the cast produces instead of the base type's physical representation.
static LogicalType GetVariantSourceType(const CastFunctionSet &casts, const LogicalType &type) {
	if (type.IsJSONType() || casts.HasCustomJSONCast(type)) {
		return LogicalType::JSON();
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return LogicalType::LIST(GetVariantSourceType(casts, ListType::GetChildType(type)));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(GetVariantSourceType(casts, ArrayType::GetChildType(type)), ArrayType::GetSize(type));
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> children;
		for (auto &entry : StructType::GetChildTypes(type)) {
			children.emplace_back(entry.first, GetVariantSourceType(casts, entry.second));
		}
		return LogicalType::STRUCT(std::move(children));
	}
	case LogicalTypeId::TUPLE: {
		vector<LogicalType> children;
		for (auto &entry : StructType::GetChildTypes(type)) {
			children.push_back(GetVariantSourceType(casts, entry.second));
		}
		return LogicalType::TUPLE(std::move(children));
	}
	case LogicalTypeId::MAP:
		return LogicalType::MAP(GetVariantSourceType(casts, MapType::KeyType(type)),
		                        GetVariantSourceType(casts, MapType::ValueType(type)));
	case LogicalTypeId::UNION: {
		child_list_t<LogicalType> children;
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			children.emplace_back(UnionType::GetMemberName(type, i),
			                      GetVariantSourceType(casts, UnionType::GetMemberType(type, i)));
		}
		return LogicalType::UNION(std::move(children));
	}
	default:
		return type;
	}
}

} // namespace variant

BoundCastInfo DefaultCasts::ImplicitToVariantCast(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	if (TypeVisitor::Contains(source, LogicalTypeId::TYPE)) {
		//! TYPE has no VARIANT representation - only NULL values can be cast
		return TryVectorNullCast;
	}
	auto has_custom_cast = TypeVisitor::Contains(
	    source, [&](const LogicalType &type) { return input.function_set.HasCustomJSONCast(type); });
	auto variant_source = source;
	unique_ptr<BoundCastInfo> pre_cast;
	if (has_custom_cast) {
		variant_source = variant::GetVariantSourceType(input.function_set, source);
		pre_cast = make_uniq<BoundCastInfo>(input.GetCastFunction(source, variant_source));
	}
	return BoundCastInfo(variant::CastToVARIANT,
	                     make_uniq<variant::VariantCastData>(variant_source, std::move(pre_cast)),
	                     variant::CastToVariantLocalState);
}

} // namespace duckdb
