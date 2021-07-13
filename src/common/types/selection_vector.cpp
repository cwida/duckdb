#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

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

void SelectionVector::Print(idx_t count) const {
	Printer::Print(ToString(count));
}
// LCOV_EXCL_STOP

buffer_ptr<SelectionData> SelectionVector::Slice(const SelectionVector &sel, idx_t count) const {
	auto data = make_buffer<SelectionData>(count);
	auto result_ptr = data->owned_data.get();
	// for every element, we perform result[i] = target[new[i]]
	for (idx_t i = 0; i < count; i++) {
		auto new_idx = sel.get_index(i);
		auto idx = this->get_index(new_idx);
		result_ptr[i] = idx;
	}
	return data;
}

} // namespace duckdb
