//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/unified_vector_format.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bitset.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/type_util.hpp"

namespace duckdb {

struct UnifiedVectorFormat {
	DUCKDB_API UnifiedVectorFormat();
	// disable copy constructors
	UnifiedVectorFormat(const UnifiedVectorFormat &other) = delete;
	UnifiedVectorFormat &operator=(const UnifiedVectorFormat &) = delete;
	//! enable move constructors
	DUCKDB_API UnifiedVectorFormat(UnifiedVectorFormat &&other) noexcept;
	DUCKDB_API UnifiedVectorFormat &operator=(UnifiedVectorFormat &&) noexcept;

	const SelectionVector *sel;
	const_data_ptr_t data;
	ValidityMask validity;
	SelectionVector owned_sel;
	PhysicalType physical_type;

	template <class T>
	void VerifyVectorType() const {
#ifdef DUCKDB_DEBUG_NO_SAFETY
		D_ASSERT(StorageTypeCompatible<T>(physical_type));
#else
		if (!StorageTypeCompatible<T>(physical_type)) {
			ThrowVectorTypeMismatch(GetTypeId<T>());
		}
#endif
	}
	[[noreturn]] DUCKDB_API void ThrowVectorTypeMismatch(PhysicalType expected) const;

	template <class T>
	static inline const T *GetDataUnsafe(const UnifiedVectorFormat &format) {
		return reinterpret_cast<const T *>(format.data);
	}
	template <class T>
	static inline const T *GetData(const UnifiedVectorFormat &format) {
		return format.GetData<T>();
	}
	template <class T>
	inline const T *GetData() const {
		VerifyVectorType<T>();
		return GetDataUnsafe<T>(*this);
	}
	template <class T>
	static inline T *GetDataNoConst(UnifiedVectorFormat &format) {
		format.VerifyVectorType<T>();
		return reinterpret_cast<T *>(format.data);
	}
};

struct RecursiveUnifiedVectorFormat {
	UnifiedVectorFormat unified;
	vector<RecursiveUnifiedVectorFormat> children;
	LogicalType logical_type;
};

struct UnifiedVariantVector {
	//! The 'keys' list (dictionary)
	DUCKDB_API static const UnifiedVectorFormat &GetKeys(const RecursiveUnifiedVectorFormat &vec);
	//! The 'keys' list entry
	DUCKDB_API static const UnifiedVectorFormat &GetKeysEntry(const RecursiveUnifiedVectorFormat &vec);
	//! The 'children' list
	DUCKDB_API static const UnifiedVectorFormat &GetChildren(const RecursiveUnifiedVectorFormat &vec);
	//! The 'keys_index' inside the 'children' list
	DUCKDB_API static const UnifiedVectorFormat &GetChildrenKeysIndex(const RecursiveUnifiedVectorFormat &vec);
	//! The 'values_index' inside the 'children' list
	DUCKDB_API static const UnifiedVectorFormat &GetChildrenValuesIndex(const RecursiveUnifiedVectorFormat &vec);
	//! The 'values' list
	DUCKDB_API static const UnifiedVectorFormat &GetValues(const RecursiveUnifiedVectorFormat &vec);
	//! The 'type_id' inside the 'values' list
	DUCKDB_API static const UnifiedVectorFormat &GetValuesTypeId(const RecursiveUnifiedVectorFormat &vec);
	//! The 'byte_offset' inside the 'values' list
	DUCKDB_API static const UnifiedVectorFormat &GetValuesByteOffset(const RecursiveUnifiedVectorFormat &vec);
	//! The binary blob 'data' encoding the Variant for the row
	DUCKDB_API static const UnifiedVectorFormat &GetData(const RecursiveUnifiedVectorFormat &vec);
};

struct ConsecutiveChildListInfo {
	ConsecutiveChildListInfo() : is_constant(true), needs_slicing(false), child_list_info(list_entry_t(0, 0)) {
	}
	bool is_constant;
	bool needs_slicing;
	list_entry_t child_list_info;
};

} // namespace duckdb
