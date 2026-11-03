//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/primitive_dictionary.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct ParquetOperatorPageState;

struct PrimitiveDictionaryTargetData {
	AllocatedData data;
	idx_t size = 0;
};

struct PrimitiveCastOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input, ParquetOperatorPageState *state) {
		return TGT(input);
	}
	template <class SRC, class TGT>
	static constexpr idx_t WriteSize(const TGT &input) {
		return sizeof(TGT);
	}
	template <class SRC, class TGT>
	static void WriteToStream(const TGT &input, WriteStream &ser) {
		ser.WriteData(const_data_ptr_cast(&input), sizeof(TGT));
	}
};

template <class SRC, class TGT = SRC, class OP = PrimitiveCastOperator>
class PrimitiveDictionary {
private:
	static constexpr idx_t LOAD_FACTOR = 2;
	//! Clear sweeps every slot once the occupied slots exceed this fraction of the capacity
	static constexpr idx_t CLEAR_SWEEP_FACTOR = 8;

	static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(-1);
	struct primitive_dictionary_entry_t {
		SRC value;
		uint32_t index;
		bool IsEmpty() const {
			return index == INVALID_INDEX;
		}
	};

public:
	static constexpr uint32_t MAXIMUM_POSSIBLE_SIZE = INVALID_INDEX - 1;
	static constexpr idx_t INITIAL_TARGET_CAPACITY = 262144;

	//! PrimitiveDictionary is a fixed-size linear probing hash table for primitive types
	//! It is used to dictionary-encode data in, e.g., Parquet files
	PrimitiveDictionary(Allocator &allocator_p, idx_t maximum_size_p, idx_t maximum_target_capacity_p)
	    : allocator(allocator_p), maximum_size(maximum_size_p), size(0),
	      capacity(NextPowerOfTwo(maximum_size * LOAD_FACTOR)), capacity_mask(capacity - 1),
	      maximum_target_capacity(maximum_target_capacity_p),
	      allocated_dictionary(allocator.Allocate(capacity * sizeof(primitive_dictionary_entry_t))),
	      allocated_target(allocator.Allocate(std::is_same<TGT, string_t>::value
	                                              ? MinValue(INITIAL_TARGET_CAPACITY, maximum_target_capacity)
	                                              : capacity * sizeof(TGT))),
	      target_stream(allocated_target.get(), allocated_target.GetSize()),
	      dictionary(reinterpret_cast<primitive_dictionary_entry_t *>(allocated_dictionary.get())), full(false) {
		ClearAllSlots();
	}

public:
	//! Insert value into dictionary (if not full)
	template <bool ADD_TO_TARGET = false>
	void Insert(SRC value) {
		if (full) {
			// nothing can be inserted anymore, so do not pay for the hash
			return;
		}
		Insert<ADD_TO_TARGET>(value, Hash(value));
	}

	//! Insert value into dictionary (if not full), reusing the hash a Lookup computed for it
	template <bool ADD_TO_TARGET = false>
	void Insert(SRC value, hash_t hash) {
		if (full) {
			return;
		}
		auto &entry = Lookup(value, hash);
		if (entry.IsEmpty()) {
			if (size + 1 > maximum_size || (ADD_TO_TARGET && !AddToTarget(value))) {
				full = true;
				return;
			}
			entry.value = value;
			entry.index = size++;
			occupied.push_back(UnsafeNumericCast<uint32_t>(&entry - dictionary));
		}
	}

	//! Get dictionary index of an already inserted value
	uint32_t GetIndex(const SRC &value) const {
		const auto &entry = Lookup(value);
		D_ASSERT(!entry.IsEmpty());
		return entry.index;
	}

	//! Iterates over inserted values
	template <typename U = SRC, typename std::enable_if<!std::is_same<U, string_t>::value, int>::type = 0>
	void IterateValues(const std::function<void(const SRC &, const TGT &)> &fun) const {
		const auto target_values = reinterpret_cast<const TGT *>(allocated_target.get());
		for (idx_t i = 0; i < capacity; i++) {
			auto &entry = dictionary[i];
			if (entry.IsEmpty()) {
				continue;
			}
			fun(entry.value, target_values[entry.index]);
		}
	}

	//! Specialized template to iterate over string_t values
	template <typename U = SRC, typename std::enable_if<std::is_same<U, string_t>::value, int>::type = 0>
	void IterateValues(const std::function<void(const SRC &, const TGT &)> &fun) const {
		for (idx_t i = 0; i < capacity; i++) {
			auto &entry = dictionary[i];
			if (entry.IsEmpty()) {
				continue;
			}
			fun(entry.value, entry.value);
		}
	}

	//! Get the number of unique values in the dictionary
	idx_t GetSize() const {
		return size;
	}

	//! If any of the inserts caused the dictionary to be full, this returns true
	bool IsFull() const {
		return full;
	}

	//! Get the target written values as a memory stream (zero-copy)
	unique_ptr<MemoryStream> GetTargetMemoryStream() const {
		auto result = make_uniq<MemoryStream>(target_stream.GetData(), target_stream.GetCapacity());
		result->SetPosition(target_stream.GetPosition());
		return result;
	}

	//! Take ownership of the target written values
	PrimitiveDictionaryTargetData TakeTargetData() {
		PrimitiveDictionaryTargetData result;
		result.size = target_stream.GetPosition();
		result.data = std::move(allocated_target);
		target_stream = MemoryStream(nullptr, 0);
		return result;
	}

	void Reset() {
		allocated_dictionary.Reset();
		allocated_target.Reset();
		occupied = vector<uint32_t>();
	}

	//! Empty the dictionary, in time proportional to the entries it holds rather than to its capacity
	void Clear() {
		if (occupied.size() * CLEAR_SWEEP_FACTOR < capacity) {
			for (auto offset : occupied) {
				dictionary[offset].index = INVALID_INDEX;
			}
		} else {
			ClearAllSlots();
		}
		occupied.clear();
		size = 0;
		full = false;
	}
	//! Look up a value in the dictionary using linear probing
	primitive_dictionary_entry_t &Lookup(const SRC &value) const {
		return Lookup(value, Hash(value));
	}

	//! Look up a value with its precomputed hash
	primitive_dictionary_entry_t &Lookup(const SRC &value, hash_t hash) const {
		auto offset = hash & capacity_mask;
		while (!dictionary[offset].IsEmpty() && dictionary[offset].value != value) {
			++offset &= capacity_mask;
		}
		return dictionary[offset];
	}

private:
	void ClearAllSlots() {
		for (idx_t i = 0; i < capacity; i++) {
			dictionary[i].index = INVALID_INDEX;
		}
	}

	//! Write a value to the target data (if source is not string)
	template <typename S = SRC, typename std::enable_if<!std::is_same<S, string_t>::value, int>::type = 0>
	bool AddToTarget(const SRC &src_value) {
		const auto tgt_value = OP::template Operation<SRC, TGT>(src_value);
		if (target_stream.GetPosition() + OP::template WriteSize<SRC, TGT>(tgt_value) > target_stream.GetCapacity()) {
			return false; // Out of capacity
		}
		OP::template WriteToStream<SRC, TGT>(tgt_value, target_stream);
		return true;
	}

	//! Write a value to the target data (if source is string)
	template <typename S = SRC, typename std::enable_if<std::is_same<S, string_t>::value, int>::type = 0>
	bool AddToTarget(SRC &src_value) {
		// If source is string, target must also be string
		const auto required_size = target_stream.GetPosition() + OP::template WriteSize<SRC, TGT>(src_value);
		if (required_size > allocated_target.GetSize()) {
			// Out of capacity, allocate a new buffer
			idx_t new_target_capacity = allocated_target.GetSize();
			while (new_target_capacity < required_size) {
				if (new_target_capacity == maximum_target_capacity) {
					return false; // Maximum capacity isn't enough
				}
				// Double the size, or add the maximum increment
				new_target_capacity += MinValue(new_target_capacity, MAXIMUM_TARGET_CAPACITY_INCREMENT);
				// Bound by maximum capacity
				new_target_capacity = MinValue(new_target_capacity, maximum_target_capacity);
			}
			auto new_allocated_target = allocator.Allocate(new_target_capacity);

			// Copy over data and replace
			const auto old_ptr = allocated_target.get();
			const auto new_ptr = new_allocated_target.get();
			memcpy(new_ptr, old_ptr, allocated_target.GetSize());
			allocated_target = std::move(new_allocated_target);

			// Also replace the stream
			MemoryStream new_target_stream(allocated_target.get(), allocated_target.GetSize());
			new_target_stream.SetPosition(target_stream.GetPosition());
			target_stream = std::move(new_target_stream);

			// Recompute string pointers from old to new buffer
			for (idx_t i = 0; i < capacity; i++) {
				auto &entry = dictionary[i];
				if (entry.IsEmpty() || entry.value.IsInlined()) {
					continue;
				}
				entry.value.SetPointer(
				    char_ptr_cast(new_ptr + (const_data_ptr_cast(entry.value.GetPointer()) - old_ptr)));
			}
		}

		const auto ptr = target_stream.GetData() + target_stream.GetPosition() + sizeof(uint32_t);
		OP::template WriteToStream<SRC, TGT>(src_value, target_stream);

		if (!src_value.IsInlined()) {
			src_value.SetPointer(char_ptr_cast(ptr));
		}

		return true;
	}

private:
	Allocator &allocator;

	//! Maximum size and current size
	const idx_t maximum_size;
	uint32_t size;

	//! Dictionary capacity (power of two) and corresponding mask
	const idx_t capacity;
	const idx_t capacity_mask;

	//! Capacity of target encoded data
	const idx_t maximum_target_capacity;
	static constexpr idx_t MAXIMUM_TARGET_CAPACITY_INCREMENT = 33554432ULL;

	//! Allocated regions for dictionary/target
	AllocatedData allocated_dictionary;
	AllocatedData allocated_target;
	MemoryStream target_stream;

	//! Pointers to allocated regions for convenience
	primitive_dictionary_entry_t *const dictionary;
	//! Offsets of the filled slots, so Clear does not have to sweep the whole capacity
	vector<uint32_t> occupied;

	//! More values inserted than possible
	bool full;
};

} // namespace duckdb
