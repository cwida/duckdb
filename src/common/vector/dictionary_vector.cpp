#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/sel_cache.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

DictionaryBuffer::DictionaryBuffer(const SelectionVector &sel, idx_t sel_count_p, buffer_ptr<DictionaryEntry> entry_p)
    : VectorBuffer(VectorType::DICTIONARY_VECTOR, VectorBufferType::DICTIONARY_BUFFER, count_t(sel_count_p)),
      sel_vector(sel), entry(std::move(entry_p)) {
}
DictionaryBuffer::DictionaryBuffer(buffer_ptr<SelectionData> data, idx_t sel_count_p,
                                   buffer_ptr<DictionaryEntry> entry_p)
    : VectorBuffer(VectorType::DICTIONARY_VECTOR, VectorBufferType::DICTIONARY_BUFFER, count_t(sel_count_p)),
      sel_vector(std::move(data)), entry(std::move(entry_p)) {
}
DictionaryBuffer::DictionaryBuffer(const SelectionVector &sel, idx_t sel_count_p)
    : VectorBuffer(VectorType::DICTIONARY_VECTOR, VectorBufferType::DICTIONARY_BUFFER, count_t(sel_count_p)),
      sel_vector(sel) {
}
DictionaryBuffer::DictionaryBuffer(buffer_ptr<SelectionData> data, idx_t sel_count_p)
    : VectorBuffer(VectorType::DICTIONARY_VECTOR, VectorBufferType::DICTIONARY_BUFFER, count_t(sel_count_p)),
      sel_vector(std::move(data)) {
}
DictionaryBuffer::DictionaryBuffer(idx_t count)
    : VectorBuffer(VectorType::DICTIONARY_VECTOR, VectorBufferType::DICTIONARY_BUFFER, count_t(count)),
      sel_vector(count) {
}

idx_t DictionaryBuffer::GetDataSize(const LogicalType &type, idx_t count) const {
	// just forward to child node
	return GetEntry().data.GetDataSize(count);
}

idx_t DictionaryBuffer::GetAllocationSize() const {
	auto size = VectorBuffer::GetAllocationSize();
	size += sel_vector.GetAllocationSize();
	return size + GetEntry().data.GetAllocationSize();
}

void DictionaryBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(vector_type == VectorType::DICTIONARY_VECTOR);
	auto &child = GetEntry().data;
	if (type != child.GetType()) {
		throw InternalException("Dictionary expression type mismatch - type %s does not match child type %s", type,
		                        child.GetType());
	}
	// consumers may read any entry in [0, dictionary size), not only the ones the selection vector references - e.g.
	// ColumnDataCollection copies the whole dictionary to keep it compressed - so verify the entire child
	child.Verify();
	if (!sel.IsSet()) {
		// sel is not set - directly pass in the dictionary
		child.Verify(sel_vector, count);
	} else {
		// sel is set - slice the dictionary with the selection vector
		SelectionVector child_sel(count);
		for (idx_t i = 0; i < count; i++) {
			child_sel.set_index(i, sel_vector.get_index(sel.get_index(i)));
		}
		child.Verify(child_sel, count);
	}
}

void DictionaryBuffer::ToUnifiedFormat(UnifiedVectorFormat &format) const {
	format.owned_sel.Initialize(sel_vector);
	format.sel = &format.owned_sel;

	auto &child = entry->data;
	if (child.GetVectorType() != VectorType::FLAT_VECTOR) {
		// flatten the child in-place
		entry->data.Flatten();
	}
	format.data = FlatVector::GetData(entry->data);
	format.validity = FlatVector::Validity(entry->data);
}

buffer_ptr<VectorBuffer> DictionaryBuffer::SliceWithCache(SelCache &cache, const LogicalType &type,
                                                          const SelectionVector &sel, idx_t count) {
	// dictionary vector: need to merge dictionaries
	// check if we have a cached entry
	auto target_data = sel_vector.data();
	auto cache_entry = cache.cache.find(target_data);
	buffer_ptr<VectorBuffer> result;
	if (cache_entry != cache.cache.end()) {
		// cached entry exists: use the cached selection vector with our dictionary entry
		auto &cached_dict = cache_entry->second->Cast<DictionaryBuffer>();
		result = make_buffer<DictionaryBuffer>(cached_dict.GetSelVector(), count, entry);
	} else {
		// no cached entry - perform the slice and store the result
		result = Slice(type, sel, count);
		cache.cache[target_data] = result;
	}
	return result;
}

buffer_ptr<VectorBuffer> DictionaryBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	// dictionary vector slice: slice the dictionary instead of stacking dictionaries
	if (type.InternalType() == PhysicalType::STRUCT) {
		throw InternalException("Struct vectors cannot be dictionary vectors");
	}
	auto count = end - offset;
	auto &sel_data = GetSelVector().sel_data();
	if (!sel_data) {
		// non-owning sel, we need to create a new selection vector to slice
		SelectionVector new_sel(count);
		for (idx_t i = 0; i < count; i++) {
			new_sel.set_index(i, sel_vector.get_index(offset + i));
		}
		return make_uniq<DictionaryBuffer>(new_sel, count, entry);
	}
	if (offset == 0) {
		// for offset = 0 all we have to do is update the count - so just create a new buffer
		return make_uniq<DictionaryBuffer>(sel_data, end, entry);
	}
	SelectionVector sliced_sel(sel_vector.data() + offset, count);
	auto result = make_uniq<DictionaryBuffer>(sliced_sel, count, entry);
	result->AddAuxiliaryData(make_uniq<SelectionDataHolder>(sel_data));
	return result;
}

buffer_ptr<VectorBuffer> DictionaryBuffer::SliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                         idx_t count) {
	// dictionary vector slice: slice the dictionary instead of stacking dictionaries
	if (type.InternalType() == PhysicalType::STRUCT) {
		throw InternalException("Struct vectors cannot be dictionary vectors");
	}
	auto sliced_dictionary = GetSelVector().Slice(sel, count);
	auto new_buffer = make_buffer<DictionaryBuffer>(std::move(sliced_dictionary), count, entry);
	return new_buffer;
}

Value DictionaryBuffer::GetValue(const LogicalType &type, idx_t index) const {
	if (index >= Size()) {
		throw InternalException("DictionaryBuffer::GetValue out of range for selection vector");
	}
	auto resolved_index = sel_vector.get_index(index);
	return entry->data.GetValue(resolved_index);
}

buffer_ptr<VectorBuffer> DictionaryBuffer::Flatten(const LogicalType &type) const {
	// flatten the child based on the selection vector stored in the dictionary
	return entry->data.Buffer().FlattenSlice(type, sel_vector, Size());
}

buffer_ptr<VectorBuffer> DictionaryBuffer::FlattenSliceInternal(const LogicalType &type,
                                                                const SelectionVector &input_sel, idx_t count) const {
	// get the selection vector to push into the child
	// if input_sel is set, we slice the dictionary by input_sel, otherwise we pass in the dict directly
	const_reference<SelectionVector> sel_ref(sel_vector);
	SelectionVector composed;
	if (input_sel.IsSet()) {
		// slice the dictionary using the provided selection vector
		composed.Initialize(count);
		for (idx_t i = 0; i < count; i++) {
			composed.set_index(i, sel_vector.get_index(input_sel.get_index(i)));
		}
		sel_ref = composed;
	}
	auto &sel = sel_ref.get();

	// flatten the child using the selection vector
	return entry->data.Buffer().FlattenSlice(type, sel, count);
}

buffer_ptr<DictionaryEntry> DictionaryVector::CreateReusableDictionary(const LogicalType &type, const idx_t &size) {
	auto entry = make_buffer<DictionaryEntry>(Vector(type, size));
	FlatVector::SetSize(entry->data, size);
	entry->id = UUID::ToString(UUID::GenerateRandomUUID());
	return entry;
}

buffer_ptr<DictionaryEntry> DictionaryVector::CreateReusableGlobalDictionary(const LogicalType &type,
                                                                             const idx_t &size) {
	auto entry = CreateReusableDictionary(type, size);
	entry->global_dictionary = true;
	return entry;
}

const Vector &DictionaryVector::GetCachedHashes(const Vector &input) {
	D_ASSERT(CanCacheHashes(input));

	const auto &entry = input.Buffer().Cast<DictionaryBuffer>().GetEntry();
	CallOnce(entry.cached_hashes_once, [&] {
		const auto dictionary_size = DictionarySize(input).GetIndex();
		auto hashes = make_uniq<Vector>(LogicalType::HASH, dictionary_size);
		VectorOperations::Hash(entry.data, *hashes);
		entry.cached_hashes = std::move(hashes);
	});
	return *entry.cached_hashes;
}

bool DictionaryBuffer::TrySerialize(Serializer &serializer, const LogicalType &type,
                                    bool compressed_serialization) const {
	auto dictionary_size = GetDictionarySize();
	if (!compressed_serialization || !dictionary_size.IsValid()) {
		return false;
	}
	auto dict = Vector::Ref(entry->data);
	if (dict.GetVectorType() != VectorType::FLAT_VECTOR) {
		return false;
	}
	auto count = Size();
	auto dict_count = dictionary_size.GetIndex();
	SelectionVector new_sel(count), used_sel(count), map_sel(dict_count);

	// dictionaries may be large (row-group level). A vector may use only a small part.
	// So, restrict dict to the used_sel subset & remap old_sel into new_sel to the new dict positions
	sel_t CODE_UNSEEN = static_cast<sel_t>(dict_count);
	for (sel_t i = 0; i < dict_count; ++i) {
		map_sel[i] = CODE_UNSEEN; // initialize with unused marker
	}
	idx_t used_count = 0;
	for (idx_t i = 0; i < count; ++i) {
		auto pos = sel_vector[i];
		if (map_sel[pos] == CODE_UNSEEN) {
			map_sel[pos] = static_cast<sel_t>(used_count);
			used_sel[used_count++] = pos;
		}
		new_sel[i] = map_sel[pos];
	}
	if (used_count * 2 >= count) {
		// only serialize as a dict vector if that makes things smaller
		return false;
	}
	auto sel_data = reinterpret_cast<data_ptr_t>(new_sel.data());
	dict.Slice(used_sel, used_count);
	serializer.WriteProperty(90, "vector_type", VectorType::DICTIONARY_VECTOR);
	serializer.WriteProperty(91, "sel_vector", sel_data, sizeof(sel_t) * count);
	serializer.WriteProperty(92, "dict_count", used_count);
	dict.Serialize(serializer, false);
	return true;
}

buffer_ptr<VectorBuffer> DictionaryBuffer::Deserialize(Deserializer &deserializer, const LogicalType &type,
                                                       idx_t count) {
	SelectionVector sel(count);
	deserializer.ReadProperty(91, "sel_vector", reinterpret_cast<data_ptr_t>(sel.data()), sizeof(sel_t) * count);
	const auto dict_count = deserializer.ReadProperty<idx_t>(92, "dict_count");
	for (idx_t i = 0; i < count; i++) {
		if (sel.get_index(i) >= dict_count) {
			throw SerializationException("Failed to deserialize dictionary vector: selection index %llu is out of "
			                             "range of the dictionary of size %llu",
			                             sel.get_index(i), dict_count);
		}
	}
	Vector dict(type, MaxValue<idx_t>(dict_count, STANDARD_VECTOR_SIZE));
	dict.Deserialize(deserializer, dict_count);
	FlatVector::SetSize(dict, dict_count);
	dict.Slice(sel, count);
	return dict.GetBufferRef();
}

} // namespace duckdb
