//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/selection_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {
class VectorBuffer;

struct SelectionData {
	explicit SelectionData(idx_t count) {
		owned_data = unique_ptr<sel_t[]>(new sel_t[count]);
	}
	unique_ptr<sel_t[]> owned_data;
};

struct SelectionVector {
	SelectionVector() : sel_vector(nullptr) {
	}
	explicit SelectionVector(sel_t *sel) {
		Initialize(sel);
	}
	explicit SelectionVector(idx_t count) {
		Initialize(count);
	}
	SelectionVector(idx_t start, idx_t count) {
		Initialize(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			set_index(i, start + i);
		}
	}
	SelectionVector(const SelectionVector &sel_vector) {
		Initialize(sel_vector);
	}
	explicit SelectionVector(buffer_ptr<SelectionData> data) {
		Initialize(move(data));
	}

public:
	void Initialize(sel_t *sel) {
		selection_data.reset();
		sel_vector = sel;
	}
	void Initialize(idx_t count = STANDARD_VECTOR_SIZE) {
		selection_data = make_buffer<SelectionData>(count);
		sel_vector = selection_data->owned_data.get();
	}
	void Initialize(buffer_ptr<SelectionData> data) {
		selection_data = move(data);
		sel_vector = selection_data->owned_data.get();
	}
	void Initialize(const SelectionVector &other) {
		selection_data = other.selection_data;
		sel_vector = other.sel_vector;
	}

	void set_index(idx_t idx, idx_t loc) {
		sel_vector[idx] = loc;
	}
	void swap(idx_t i, idx_t j) {
		sel_t tmp = sel_vector[i];
		sel_vector[i] = sel_vector[j];
		sel_vector[j] = tmp;
	}
	idx_t get_index(idx_t idx) const {
		return sel_vector ? sel_vector[idx] : idx;
	}
	sel_t *data() {
		return sel_vector;
	}
	buffer_ptr<SelectionData> sel_data() {
		return selection_data;
	}
	buffer_ptr<SelectionData> Slice(const SelectionVector &sel, idx_t count) const;

	string ToString(idx_t count = 0) const;
	void Print(idx_t count = 0) const;

	sel_t &operator[](idx_t index) {
		return sel_vector[index];
	}

private:
	sel_t *sel_vector;
	buffer_ptr<SelectionData> selection_data;
};

class OptionalSelection {
public:
	explicit inline OptionalSelection(SelectionVector *sel_p) : sel(sel_p) {

		if (sel) {
			vec.Initialize(sel->data());
			sel = &vec;
		}
	}

	inline operator SelectionVector *() {
		return sel;
	}

	inline void Append(idx_t &count, const idx_t idx) {
		if (sel) {
			sel->set_index(count, idx);
		}
		++count;
	}

	inline void Advance(idx_t completed) {
		if (sel) {
			sel->Initialize(sel->data() + completed);
		}
	}

private:
	SelectionVector *sel;
	SelectionVector vec;
};

} // namespace duckdb
