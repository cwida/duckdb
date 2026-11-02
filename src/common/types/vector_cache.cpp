#include "duckdb/common/types/vector_cache.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"

namespace duckdb {

class VectorCacheEntry {
public:
	explicit VectorCacheEntry(Allocator &allocator, const LogicalType &type_p, idx_t capacity_p = STANDARD_VECTOR_SIZE)
	    : type(type_p), capacity(capacity_p) {
		auto internal_type = type.InternalType();
		switch (internal_type) {
		case PhysicalType::LIST: {
			// child data of the list
			auto &child_type = ListType::GetChildType(type);
			child_caches.push_back(make_uniq<VectorCacheEntry>(allocator, child_type, capacity));
			auto child_vector = make_uniq<Vector>(child_type, nullptr);
			buffer = make_buffer<VectorListBuffer>(AllocatedData(allocator), capacity, std::move(child_vector));
			break;
		}
		case PhysicalType::ARRAY: {
			auto &child_type = ArrayType::GetChildType(type);
			auto array_size = ArrayType::GetSize(type);
			child_caches.push_back(make_uniq<VectorCacheEntry>(allocator, child_type, array_size * capacity));
			auto child_vector = make_uniq<Vector>(child_type, nullptr);
			buffer = make_shared_ptr<VectorArrayBuffer>(std::move(child_vector), array_size, capacity);
			break;
		}
		case PhysicalType::STRUCT: {
			auto &child_types = StructType::GetChildTypes(type);
			vector<Vector> children;
			for (auto &child_type : child_types) {
				child_caches.push_back(make_uniq<VectorCacheEntry>(allocator, child_type.second, capacity));
				children.emplace_back(child_type.second, nullptr);
			}
			buffer = make_buffer<VectorStructBuffer>(std::move(children), capacity);
			break;
		}
		case PhysicalType::VARCHAR:
			buffer = make_buffer<VectorStringBuffer>(AllocatedData(allocator), capacity);
			break;
		default:
			buffer =
			    make_buffer<StandardVectorBuffer>(AllocatedData(allocator), capacity, GetTypeIdSize(internal_type));
			break;
		}
	}

	void ResetFromCache(Vector &result) {
		D_ASSERT(type == result.GetType());
		auto internal_type = type.InternalType();
		buffer->ClearAuxiliaryData();
		buffer->ResetCapacity(buffer->Capacity());
		buffer->SetVectorTypeOnly(VectorType::FLAT_VECTOR);
		buffer->SetVectorSizeOnly(0);
		switch (internal_type) {
		case PhysicalType::LIST: {
			child_caches[0]->ResetFromCache(buffer->Cast<VectorListBuffer>().GetChild());
			break;
		}
		case PhysicalType::ARRAY: {
			child_caches[0]->ResetFromCache(buffer->Cast<VectorArrayBuffer>().GetChild());
			break;
		}
		case PhysicalType::STRUCT: {
			auto &children = buffer->Cast<VectorStructBuffer>().GetChildren();
			for (idx_t i = 0; i < children.size(); i++) {
				child_caches[i]->ResetFromCache(children[i]);
			}
			break;
		}
		default:
			break;
		}
		result.SetBuffer(buffer);
	}

	const LogicalType &GetType() {
		return type;
	}
	idx_t GetCapacity() const {
		return capacity;
	}

private:
	//! The type of the vector cache
	LogicalType type;
	//! Child caches (if any). Used for nested types.
	vector<unique_ptr<VectorCacheEntry>> child_caches;
	//! Buffer data for the vector (if any)
	buffer_ptr<VectorBuffer> buffer;
	//! Capacity of the vector
	capacity_t capacity;
};

VectorCache::VectorCache() {
}

VectorCache::VectorCache(Allocator &allocator, const LogicalType &type_p, const idx_t capacity_p) {
	cache_entry = make_uniq<VectorCacheEntry>(allocator, type_p, capacity_p);
}

VectorCache::~VectorCache() {
}

VectorCache::VectorCache(VectorCache &&other) noexcept : cache_entry(std::move(other.cache_entry)) {
}

VectorCache &VectorCache::operator=(VectorCache &&other) noexcept {
	cache_entry = std::move(other.cache_entry);
	return *this;
}

void VectorCache::ResetFromCache(Vector &result) const {
	if (!cache_entry) {
		// uninitialized column: drop the last reference so it does not keep a stale size
		result.SetBuffer(nullptr);
		return;
	}
	cache_entry->ResetFromCache(result);
}

const LogicalType &VectorCache::GetType() const {
	return cache_entry->GetType();
}

optional_idx VectorCache::GetCapacity() const {
	if (!cache_entry) {
		return optional_idx();
	}
	return optional_idx(cache_entry->GetCapacity());
}

} // namespace duckdb
