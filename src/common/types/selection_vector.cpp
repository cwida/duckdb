#include "duckdb/common/types/selection_vector.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/bswap.hpp"

namespace duckdb {

SelectionData::SelectionData(idx_t count) {
	owned_data = Allocator::DefaultAllocator().Allocate(MaxValue<idx_t>(count, 1) * sizeof(sel_t));
#ifdef DEBUG
	auto data_ptr = reinterpret_cast<sel_t *>(owned_data.get());
	for (idx_t i = 0; i < count; i++) {
		data_ptr[i] = std::numeric_limits<sel_t>::max();
	}
#endif
}

SelectionData::~SelectionData() = default;

namespace {
struct BooleanSelectionTable {
	sel_t positions[256][8] {};
	uint8_t counts[256] {};

	constexpr BooleanSelectionTable() {
		for (idx_t mask = 0; mask < 256; mask++) {
			for (idx_t bit = 0; bit < 8; bit++) {
				if (mask & (1U << bit)) {
					positions[mask][counts[mask]++] = static_cast<sel_t>(bit);
				}
			}
		}
	}
};

constexpr BooleanSelectionTable BOOLEAN_SELECTION_TABLE;

struct SelectedBooleanValues {
	const bool *matches;
	const sel_t *selection;

	bool operator[](idx_t index) const {
		return matches[selection[index]];
	}
	SelectedBooleanValues operator+(idx_t offset) const {
		return {matches, selection + offset};
	}
};

template <class T>
uint64_t BooleanSelectionWord(T matches) {
	// Pack values without depending on bool's object representation.
	uint8_t bytes[8];
	for (idx_t i = 0; i < 8; i++) {
		bytes[i] = matches[i];
	}
	return BSwapIfBE(Load<uint64_t>(bytes));
}

template <class T>
uint8_t BooleanSelectionMask(T matches) {
	// Pack eight 0/1 bytes into their corresponding bits.
	const auto word = BooleanSelectionWord(matches);
	return static_cast<uint8_t>((word * UINT64_C(0x0102040810204080)) >> 56);
}

void WriteBooleanSelection(sel_t *result, sel_t base, uint8_t mask) {
	// Stage the positions so the compiler can vectorize the writes.
	sel_t positions[8];
	for (idx_t j = 0; j < 8; j++) {
		positions[j] = base + BOOLEAN_SELECTION_TABLE.positions[mask][j];
	}
	memcpy(result, positions, sizeof(positions));
}

template <class T, bool MATCH>
idx_t SelectFromBooleanLoop(T matches, idx_t count, sel_t *result, idx_t offset) {
	idx_t result_count = 0;
	idx_t i = 0;
	for (; i + 8 <= count; i += 8) {
		const auto mask = BooleanSelectionMask(matches + i) ^ (MATCH ? 0 : 255);
		const auto base = UnsafeNumericCast<sel_t>(offset + i);
		WriteBooleanSelection(result + result_count, base, mask);
		result_count += BOOLEAN_SELECTION_TABLE.counts[mask];
	}
	for (; i < count; i++) {
		result[result_count] = UnsafeNumericCast<sel_t>(offset + i);
		result_count += MATCH ? matches[i] : !matches[i];
	}
	return result_count;
}

template <class T>
idx_t PartitionFromBooleanLoop(T matches, idx_t count, sel_t *true_result, sel_t *false_result, idx_t offset) {
	idx_t true_count = 0;
	idx_t i = 0;
	for (; i + 8 <= count; i += 8) {
		const auto mask = BooleanSelectionMask(matches + i);
		const auto base = UnsafeNumericCast<sel_t>(offset + i);
		const auto false_count = i - true_count;
		WriteBooleanSelection(true_result + true_count, base, mask);
		WriteBooleanSelection(false_result + false_count, base, mask ^ 255);
		true_count += BOOLEAN_SELECTION_TABLE.counts[mask];
	}
	for (; i < count; i++) {
		true_result[true_count] = UnsafeNumericCast<sel_t>(offset + i);
		false_result[i - true_count] = UnsafeNumericCast<sel_t>(offset + i);
		true_count += matches[i];
	}
	return true_count;
}

template <class T>
idx_t SelectFromBooleanInternal(T matches, idx_t count, sel_t *true_result, sel_t *false_result, idx_t offset) {
	if (!false_result) {
		return SelectFromBooleanLoop<T, true>(matches, count, true_result, offset);
	}
	if (!true_result) {
		return count - SelectFromBooleanLoop<T, false>(matches, count, false_result, offset);
	}
	return PartitionFromBooleanLoop(matches, count, true_result, false_result, offset);
}
} // namespace

idx_t SelectionVector::SelectFromBoolean(const bool *matches, optional_ptr<const SelectionVector> match_sel,
                                         idx_t count, optional<array_ptr<sel_t>> true_sel,
                                         optional<array_ptr<sel_t>> false_sel, idx_t offset) {
	if (true_sel && DUCKDB_UNLIKELY(count > true_sel->size())) {
		ThrowArrayPtrSubArrayOutOfBounds(0, count, true_sel->size());
	}
	if (false_sel && DUCKDB_UNLIKELY(count > false_sel->size())) {
		ThrowArrayPtrSubArrayOutOfBounds(0, count, false_sel->size());
	}
	const auto indexed = match_sel && match_sel->IsSet();
	if (indexed && DUCKDB_UNLIKELY(count > match_sel->Capacity())) {
		ThrowArrayPtrSubArrayOutOfBounds(0, count, match_sel->Capacity());
	}
	if (count == 0) {
		return 0;
	}
	if (DUCKDB_UNLIKELY(!true_sel && !false_sel)) {
		throw InternalException("SelectFromBoolean requires at least one output selection");
	}
	auto true_result = true_sel ? true_sel->data() : nullptr;
	auto false_result = false_sel ? false_sel->data() : nullptr;
	if (indexed) {
		return SelectFromBooleanInternal(SelectedBooleanValues {matches, match_sel->data()}, count, true_result,
		                                 false_result, offset);
	}
	return SelectFromBooleanInternal(matches, count, true_result, false_result, offset);
}

// LCOV_EXCL_START
string SelectionVector::ToString(idx_t count) const {
	string result = "Selection Vector (" + to_string(count) + ") [";
	for (idx_t i = 0; i < count; i++) {
		if (i != 0) {
			result += ", ";
		}
		result += to_string(get_index(i));
	}
	result += "]";
	return result;
}

void SelectionVector::Sort(idx_t count) {
	std::sort(sel_vector, sel_vector + count);
}

void SelectionVector::ShiftLeft(const idx_t offset, const idx_t count) {
	D_ASSERT(sel_vector);
	D_ASSERT(offset + count <= capacity);
	if (offset == 0) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		sel_vector[i] = sel_vector[i + offset];
	}
}

void SelectionVector::Print(idx_t count) const {
	Printer::Print(ToString(count));
}
// LCOV_EXCL_STOP

buffer_ptr<SelectionData> SelectionVector::Slice(const SelectionVector &sel, idx_t count) const {
	auto data = make_buffer<SelectionData>(count);
	auto result_ptr = reinterpret_cast<sel_t *>(data->owned_data.get());
	// for every element, we perform result[i] = target[new[i]]
	for (idx_t i = 0; i < count; i++) {
		auto new_idx = sel.get_index(i);
		auto idx = this->get_index(new_idx);
		result_ptr[i] = UnsafeNumericCast<sel_t>(idx);
	}
	return data;
}

idx_t SelectionVector::SliceInPlace(const SelectionVector &source, idx_t count) {
	for (idx_t i = 0; i < count; ++i) {
		set_index(i, get_index(source.get_index(i)));
	}

	return count;
}

void SelectionVector::Verify(idx_t count, idx_t vector_size) const {
#ifdef DEBUG
	D_ASSERT(vector_size >= 1);
	for (idx_t i = 0; i < count; i++) {
		auto index = get_index(i);
		if (index >= vector_size) {
			throw InternalException(
			    "Provided SelectionVector is invalid, index %d points to %d, which is out of range. "
			    "the valid range (0-%d)",
			    i, index, vector_size - 1);
		}
	}
#endif
}

idx_t SelectionVector::GetAllocationSize() const {
	if (!selection_data) {
		return 0;
	}
	return selection_data->owned_data.GetSize();
}

} // namespace duckdb
