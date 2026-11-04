//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/dictionary_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/once_flag.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! The DictionaryEntry holds a child Vector for dictionary-encoded vectors
class DictionaryEntry {
public:
	explicit DictionaryEntry(Vector vector) : data(std::move(vector)) {
	}

public:
	Vector data;
	//! Optional id to uniquely identify re-occurring dictionaries
	string id;
	//! True iff the producer wraps this same entry in every output chunk for its lifetime (stable id, no flat
	//! fall-through), making it a global dictionary. Set only via CreateReusableGlobalDictionary.
	bool global_dictionary = false;
	//! For caching the hashes of a child buffer (mutable: cache is logically const)
	mutable OnceFlag cached_hashes_once;
	mutable unique_ptr<Vector> cached_hashes;
};

//! The DictionaryBuffer holds a selection vector and a reference to a DictionaryEntry
class DictionaryBuffer : public VectorBuffer {
public:
	explicit DictionaryBuffer(const SelectionVector &sel, idx_t sel_count, buffer_ptr<DictionaryEntry> entry_p);
	explicit DictionaryBuffer(buffer_ptr<SelectionData> data, idx_t sel_count, buffer_ptr<DictionaryEntry> entry_p);
	explicit DictionaryBuffer(const SelectionVector &sel, idx_t sel_count);
	explicit DictionaryBuffer(buffer_ptr<SelectionData> data, idx_t sel_count);
	explicit DictionaryBuffer(idx_t count = STANDARD_VECTOR_SIZE);

public:
	idx_t Capacity() const override {
		return Size();
	}
	const SelectionVector &GetSelVector() const {
		return sel_vector;
	}
	SelectionVector &GetSelVector() {
		return sel_vector;
	}
	void SetSelVector(const SelectionVector &vector) {
		this->sel_vector.Initialize(vector);
	}
	optional_idx GetDictionarySize() const {
		if (entry->data.size() == 0) {
			// FIXME: we should be directly returning entry->data.size(), this should not be an optional_idx
			return optional_idx();
		}
		return entry->data.size();
	}
	void SetDictionaryId(string id) {
		entry->id = std::move(id);
	}
	const string &GetDictionaryId() const {
		return entry->id;
	}

	DictionaryEntry &GetEntry() {
		return *entry;
	}
	const DictionaryEntry &GetEntry() const {
		return *entry;
	}
	buffer_ptr<DictionaryEntry> GetEntryPtr() {
		return entry;
	}
	void SetEntry(buffer_ptr<DictionaryEntry> entry_p) {
		entry = std::move(entry_p);
	}

public:
	idx_t GetDataSize(const LogicalType &type, idx_t count) const override;
	idx_t GetAllocationSize() const override;
	void ToUnifiedFormat(UnifiedVectorFormat &format) const override;
	buffer_ptr<VectorBuffer> Flatten(const LogicalType &type) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;
	buffer_ptr<VectorBuffer> SliceWithCache(SelCache &cache, const LogicalType &type, const SelectionVector &sel,
	                                        idx_t count) override;
	bool TrySerialize(Serializer &serializer, const LogicalType &type, bool compressed_serialization) const override;
	static buffer_ptr<VectorBuffer> Deserialize(Deserializer &deserializer, const LogicalType &type, idx_t count);

protected:
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, idx_t offset, idx_t end) override;
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) override;
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
	void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const override;

private:
	SelectionVector sel_vector;
	buffer_ptr<DictionaryEntry> entry;
};

class SelectionDataHolder : public AuxiliaryDataHolder {
public:
	explicit SelectionDataHolder(buffer_ptr<SelectionData> selection_data_p)
	    : selection_data(std::move(selection_data_p)) {
	}

private:
	buffer_ptr<SelectionData> selection_data;
};

struct DictionaryVector {
	static void VerifyDictionary(const Vector &vector) {
#ifdef DUCKDB_DEBUG_NO_SAFETY
		D_ASSERT(vector.GetVectorType() == VectorType::DICTIONARY_VECTOR);
#else
		if (vector.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
			throw InternalException(
			    "Operation requires a dictionary vector but a non-dictionary vector was encountered");
		}
#endif
	}
	static inline const SelectionVector &SelVector(const Vector &vector) {
		VerifyDictionary(vector);
		return vector.Buffer().Cast<DictionaryBuffer>().GetSelVector();
	}
	static inline SelectionVector &SelVector(Vector &vector) {
		VerifyDictionary(vector);
		return vector.BufferMutable().Cast<DictionaryBuffer>().GetSelVector();
	}
	static inline const Vector &Child(const Vector &vector) {
		VerifyDictionary(vector);
		return vector.Buffer().Cast<DictionaryBuffer>().GetEntry().data;
	}
	static inline Vector &Child(Vector &vector) {
		VerifyDictionary(vector);
		return vector.BufferMutable().Cast<DictionaryBuffer>().GetEntry().data;
	}
	static inline optional_idx DictionarySize(const Vector &vector) {
		VerifyDictionary(vector);
		const auto &dict_buffer = vector.Buffer().Cast<DictionaryBuffer>();
		return dict_buffer.GetDictionarySize();
	}
	static inline const string &DictionaryId(const Vector &vector) {
		VerifyDictionary(vector);
		const auto &dict_buffer = vector.Buffer().Cast<DictionaryBuffer>();
		return dict_buffer.GetDictionaryId();
	}
	static inline bool CanCacheHashes(const LogicalType &type) {
		return type.InternalType() == PhysicalType::VARCHAR;
	}
	static inline bool CanCacheHashes(const Vector &vector) {
		return DictionarySize(vector).IsValid() && !DictionaryId(vector).empty() && CanCacheHashes(vector.GetType());
	}
	static buffer_ptr<DictionaryEntry> CreateReusableDictionary(const LogicalType &type, const idx_t &size);
	//! Mint a reusable dictionary entry whose lifetime spans the entire producing operator instance
	static buffer_ptr<DictionaryEntry> CreateReusableGlobalDictionary(const LogicalType &type, const idx_t &size);
	//! True iff vector is a DICTIONARY_VECTOR whose entry is a global dictionary
	static inline bool IsGlobalDictionary(const Vector &vector) {
		if (vector.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
			return false;
		}
		return vector.Buffer().Cast<DictionaryBuffer>().GetEntry().global_dictionary;
	}
	static const Vector &GetCachedHashes(const Vector &input);
};

} // namespace duckdb
