#include "duckdb/execution/perfect_hash_join_table.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"
#include "duckdb/execution/ht_entry.hpp"

namespace duckdb {

PerfectHashJoinTable::~PerfectHashJoinTable() {
}

void PerfectHashJoinTable::InitializeEntries(idx_t from, idx_t to) {
	memset(entries.get() + from * sizeof(uint64_t), 0, (to - from) * sizeof(uint64_t));
}

void PerfectHashJoinTable::ConvertToDictionary(idx_t pointer_offset) {
	D_ASSERT(type == Type::ROWS);
	auto directory = GetEntries();
	for (auto &entry : directory) {
		if (entry) {
			// One-based dictionary positions preserve zero as the miss sentinel.
			entry = static_cast<uint64_t>(Load<uint32_t>(cast_uint64_to_pointer(entry) + pointer_offset)) + 1;
		}
	}
	type = Type::DICTIONARY;
}

template <class T>
static uhugeint_t KeyDistance(T max, T min) {
	return uhugeint_t(0, static_cast<uint64_t>(max) - static_cast<uint64_t>(min));
}

template <>
uhugeint_t KeyDistance(hugeint_t max, hugeint_t min) {
	return uhugeint_t(static_cast<uint64_t>(max.upper) - static_cast<uint64_t>(min.upper) - (max.lower < min.lower),
	                  max.lower - min.lower);
}

template <>
uhugeint_t KeyDistance(uhugeint_t max, uhugeint_t min) {
	return uhugeint_t(max.upper - min.upper - (max.lower < min.lower), max.lower - min.lower);
}

template <class T>
class TypedPerfectHashJoinTable : public PerfectHashJoinTable {
public:
	TypedPerfectHashJoinTable(Type type, bool nulls_equal) : PerfectHashJoinTable(type, nulls_equal) {
	}

	void UpdateBounds(const Vector &keys) override {
		if (range_too_large) {
			return;
		}
		auto values = keys.Values<T>();
		const auto count =
		    keys.GetVectorType() == VectorType::CONSTANT_VECTOR ? MinValue<idx_t>(keys.size(), 1) : keys.size();
		T chunk_min {};
		T chunk_max {};
		bool has_values = false;
		for (idx_t i = 0; i < count; i++) {
			auto entry = values[i];
			if (entry.IsValid()) {
				const auto value = entry.GetValue();
				chunk_min = has_values ? MinValue(chunk_min, value) : value;
				chunk_max = has_values ? MaxValue(chunk_max, value) : value;
				has_values = true;
			}
		}
		if (has_values) {
			WidenBounds(chunk_min, chunk_max);
		}
		CheckRange();
	}

	void MergeBounds(const PerfectHashJoinTable &other_p) override {
		auto &other = static_cast<const TypedPerfectHashJoinTable &>(other_p);
		range_too_large = range_too_large || other.range_too_large;
		if (!range_too_large && other.has_bounds) {
			WidenBounds(other.min, other.max);
			CheckRange();
		}
	}

	bool Initialize(Allocator &allocator, idx_t build_count, idx_t pointer_table_size) override {
		if (range_too_large || build_count == 0) {
			return false;
		}
		// Reserve one additional slot for NULL, including an all-NULL build.
		domain_size = has_bounds ? KeyDistance(max, min).lower + 1 : 0;
		const auto slots = domain_size + 1;
		const auto bytes = IsBitmap() ? ((slots + 63) / 64) * sizeof(uint64_t) : slots * sizeof(uint64_t);
		const auto budget = IsBitmap() ? MinValue<idx_t>(MAX_BYTES / sizeof(uint64_t), build_count) * sizeof(uint64_t)
		                               : MinValue<idx_t>(MAX_BYTES, pointer_table_size);
		if (bytes > budget) {
			return false;
		}
		entries = allocator.Allocate(bytes);
		return true;
	}

	bool Insert(const Vector &rows, const TupleDataLayout &layout, idx_t pointer_offset, bool parallel) override {
		auto row_values = rows.Values<data_ptr_t>();
		const auto key_offset = layout.GetOffsets()[0];
		const auto column_count = layout.ColumnCount();
		auto directory = GetEntries();
		auto atomic_directory = GetAtomicEntries();
		bool has_chains = false;
		for (auto entry : row_values) {
			auto row = entry.GetValue();
			const auto valid = TemplatedValidityMask<uint8_t>(row, column_count).RowIsValid(0);
			if (!valid && !nulls_equal) {
				continue;
			}
			const auto slot = valid ? GetIndex(Load<T>(row + key_offset)) : domain_size;
			D_ASSERT(slot <= domain_size);
			if (IsBitmap()) {
				const auto mask = uint64_t(1) << (slot % 64);
				if (parallel) {
					auto &word = atomic_directory[slot / 64];
					if (!(word.load(std::memory_order_relaxed) & mask)) {
						word.fetch_or(mask, std::memory_order_relaxed);
					}
				} else {
					directory[slot / 64] |= mask;
				}
			} else {
				const auto pointer = cast_pointer_to_uint64(row);
				const auto previous =
				    parallel ? atomic_directory[slot].exchange(pointer, std::memory_order_relaxed) : directory[slot];
				if (!parallel) {
					directory[slot] = pointer;
				}
				Store<uint64_t>(previous, row + pointer_offset);
				has_chains = has_chains || previous != 0;
			}
		}
		return has_chains;
	}

	void GetRowPointers(const Vector &keys, optional_ptr<const SelectionVector> sel, idx_t &count, Vector &pointers,
	                    SelectionVector &matches) const override {
		D_ASSERT(GetType() == Type::ROWS);
		auto values = keys.Values<T>();
		auto directory = GetEntries();
		FlatVector::SetSize(pointers, count_t(keys.size()));
		auto writer = FlatVector::ScatterWriter<data_ptr_t>(pointers);
		idx_t match_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto row = sel ? sel->get_index(i) : i;
			auto entry = values[row];
			uint64_t pointer = 0;
			if (!entry.IsValid()) {
				pointer = directory[domain_size];
			} else if (InRange(entry.GetValue())) {
				pointer = directory[GetIndex(entry.GetValue())];
			}
			writer[row] = cast_uint64_to_pointer(pointer);
			if (pointer) {
				matches.set_index(match_count++, row);
			}
		}
		count = match_count;
	}

	idx_t ProbeMembership(const Vector &keys, unsafe_unique_array<bool> &found_match) const override {
		D_ASSERT(IsBitmap());
		D_ASSERT(keys.size() <= STANDARD_VECTOR_SIZE);
		auto values = keys.Values<T>();
		if (ShouldPrefetchMembership(values)) {
			return ProbeMembershipPrefetched(values, found_match);
		}
		auto directory = GetEntries();
		idx_t match_count = 0;
		for (idx_t i = 0; i < values.size(); i++) {
			auto entry = values[i];
			bool found = false;
			if (!entry.IsValid()) {
				found = Contains(directory, domain_size);
			} else if (InRange(entry.GetValue())) {
				found = Contains(directory, GetIndex(entry.GetValue()));
			}
			found_match[i] = found;
			match_count += found;
		}
		return match_count;
	}

	idx_t ProbeDictionary(const Vector &keys, SelectionVector &probe_sel, SelectionVector &build_sel) const override {
		D_ASSERT(GetType() == Type::DICTIONARY);
		auto values = keys.Values<T>();
		auto directory = GetEntries();
		idx_t count = 0;
		for (idx_t i = 0; i < values.size(); i++) {
			auto entry = values[i];
			uint64_t position = 0;
			if (!entry.IsValid()) {
				position = directory[domain_size];
			} else if (InRange(entry.GetValue())) {
				position = directory[GetIndex(entry.GetValue())];
			}
			if (position) {
				probe_sel.set_index(count, i);
				build_sel.set_index(count++, position - 1);
			}
		}
		return count;
	}

private:
	bool ShouldPrefetchMembership(const VectorIterator<T> &keys) const {
#if defined(__GNUC__) || defined(__clang__)
		if (keys.size() < PREFETCH_MIN_COUNT || EntryCount() * sizeof(uint64_t) <= PREFETCH_MIN_BYTES) {
			return false;
		}
		constexpr idx_t SAMPLE_COUNT = 32;
		array<idx_t, SAMPLE_COUNT> sampled_words;
		idx_t sample_count = 0;
		idx_t word_variation = 0;
		const auto step = (keys.size() + SAMPLE_COUNT - 1) / SAMPLE_COUNT;
		idx_t min_word = EntryCount();
		idx_t max_word = 0;
		for (idx_t i = 0; i < keys.size(); i += step) {
			const auto entry = keys[i];
			if (!entry.IsValid() || !InRange(entry.GetValue())) {
				continue;
			}
			const auto word = GetIndex(entry.GetValue()) / 64;
			// Reject repeated sampled words to avoid prefetching hot keys.
			for (idx_t j = 0; j < sample_count; j++) {
				if (sampled_words[j] == word) {
					return false;
				}
			}
			sampled_words[sample_count++] = word;
			word_variation |= word ^ sampled_words[0];
			min_word = MinValue(min_word, word);
			max_word = MaxValue(max_word, word);
		}
		if (sample_count < SAMPLE_COUNT / 2) {
			return false;
		}
		// Estimate distinct words after accounting for power-of-two spacing.
		D_ASSERT(word_variation != 0);
		const auto word_stride = word_variation & (idx_t(0) - word_variation);
		return (max_word - min_word) / word_stride >= PREFETCH_MIN_BYTES / sizeof(uint64_t);
#else
		return false;
#endif
	}

	template <bool HAS_NULL>
	void PrepareMembershipPositions(const VectorIterator<T> &keys,
	                                array<uint32_t, STANDARD_VECTOR_SIZE> &positions) const {
		D_ASSERT(has_bounds);
		for (idx_t i = 0; i < keys.size(); i++) {
			const auto entry = keys[i];
			if (HAS_NULL && !entry.IsValid()) {
				positions[i] = UnsafeNumericCast<uint32_t>(domain_size);
				continue;
			}
			// Unsigned distance rejects keys outside the bounded domain.
			const auto distance = KeyDistance(entry.GetValueUnsafe(), min);
			const uint8_t valid = distance.upper == 0 && distance.lower < domain_size;
			// A miss carries the high bit and addresses word zero.
			positions[i] = valid ? UnsafeNumericCast<uint32_t>(distance.lower) : MISS_BIT;
		}
	}

	static idx_t MembershipWord(uint32_t position) {
		return (position & ~MISS_BIT) / 64;
	}

	idx_t ProbeMembershipPrefetched(const VectorIterator<T> &keys, unsafe_unique_array<bool> &found_match) const {
		array<uint32_t, STANDARD_VECTOR_SIZE> positions;
		if (keys.CanHaveNull()) {
			PrepareMembershipPositions<true>(keys, positions);
		} else {
			PrepareMembershipPositions<false>(keys, positions);
		}
		auto directory = GetEntries();
		idx_t match_count = 0;
		for (idx_t i = 0; i < keys.size(); i++) {
			const auto position = positions[i];
			const auto bit = (directory[MembershipWord(position)] >> (position % 64)) & 1ULL;
			const bool found = bit & (position < MISS_BIT);
			found_match[i] = found;
			match_count += found;
			PrefetchHashTableEntry(i, keys.size(),
			                       [&](idx_t next_idx) { return &directory[MembershipWord(positions[next_idx])]; });
		}
		return match_count;
	}

	void WidenBounds(T new_min, T new_max) {
		min = has_bounds ? MinValue(min, new_min) : new_min;
		max = has_bounds ? MaxValue(max, new_max) : new_max;
		has_bounds = true;
	}

	void CheckRange() {
		const auto max_slots = IsBitmap() ? MAX_BYTES * 8 : MAX_BYTES / sizeof(uint64_t);
		if (has_bounds && KeyDistance(max, min) >= uhugeint_t(max_slots - 1)) {
			range_too_large = true;
		}
	}

	bool InRange(T value) const {
		return has_bounds && value >= min && value <= max;
	}

	idx_t GetIndex(T value) const {
		D_ASSERT(InRange(value));
		return KeyDistance(value, min).lower;
	}

	static bool Contains(const array_ptr<const uint64_t> &directory, idx_t slot) {
		return (directory[slot / 64] & (uint64_t(1) << (slot % 64))) != 0;
	}

private:
	static constexpr idx_t PREFETCH_MIN_BYTES = 1024ULL * 1024ULL;
	static constexpr idx_t PREFETCH_MIN_COUNT = 128;
	static constexpr uint32_t MISS_BIT = uint32_t(1) << 31;
	static_assert(MAX_BYTES * 8 < MISS_BIT, "Bitmap slots must leave the high bit available");

	T min {};
	T max {};
	bool has_bounds = false;
	bool range_too_large = false;
	idx_t domain_size = 0;
};

unique_ptr<PerfectHashJoinTable> PerfectHashJoinTable::Create(PhysicalType key_type, Type type, bool nulls_equal) {
	switch (key_type) {
	case PhysicalType::INT8:
		return make_uniq<TypedPerfectHashJoinTable<int8_t>>(type, nulls_equal);
	case PhysicalType::INT16:
		return make_uniq<TypedPerfectHashJoinTable<int16_t>>(type, nulls_equal);
	case PhysicalType::INT32:
		return make_uniq<TypedPerfectHashJoinTable<int32_t>>(type, nulls_equal);
	case PhysicalType::INT64:
		return make_uniq<TypedPerfectHashJoinTable<int64_t>>(type, nulls_equal);
	case PhysicalType::INT128:
		return make_uniq<TypedPerfectHashJoinTable<hugeint_t>>(type, nulls_equal);
	case PhysicalType::UINT8:
		return make_uniq<TypedPerfectHashJoinTable<uint8_t>>(type, nulls_equal);
	case PhysicalType::UINT16:
		return make_uniq<TypedPerfectHashJoinTable<uint16_t>>(type, nulls_equal);
	case PhysicalType::UINT32:
		return make_uniq<TypedPerfectHashJoinTable<uint32_t>>(type, nulls_equal);
	case PhysicalType::UINT64:
		return make_uniq<TypedPerfectHashJoinTable<uint64_t>>(type, nulls_equal);
	case PhysicalType::UINT128:
		return make_uniq<TypedPerfectHashJoinTable<uhugeint_t>>(type, nulls_equal);
	default:
		return nullptr;
	}
}

} // namespace duckdb
