#include "duckdb/execution/operator/csv_scanner/set_columns.hpp"
#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

SetColumns::SetColumns(const vector<LogicalType> *types_p, const vector<Identifier> *names_p)
    : types(types_p), names(names_p) {
	if (!types) {
		D_ASSERT(!types && !names);
	} else {
		D_ASSERT(types->size() == names->size());
	}
}

SetColumns::SetColumns() {
}

bool SetColumns::IsSet() const {
	if (!types) {
		return false;
	}
	return !types->empty();
}

idx_t SetColumns::Size() const {
	if (!types) {
		return 0;
	}
	return types->size();
}

bool SetColumns::IsCandidateUnacceptable(const idx_t num_cols, bool null_padding, bool ignore_errors,
                                         bool last_value_always_empty) const {
	if (!IsSet() || ignore_errors) {
		// We can't say its unacceptable if it's not set or if we ignore errors
		return false;
	}
	idx_t size = Size();
	// If the columns are set and there is a mismatch with the expected number of columns, with null_padding and
	// ignore_errors not set, we don't have a suitable candidate.
	// Note that we compare with max_columns_found + 1, because some broken files have the behaviour where two
	// columns are represented as: | col 1 | col_2 |
	if (num_cols == size || num_cols == size + last_value_always_empty) {
		// Good Candidate
		return false;
	}
	// if we detected more columns than we have set, it's all good because we can null-pad them
	if (null_padding && num_cols > size) {
		return false;
	}
	// Unacceptable
	return true;
}

string SetColumns::ToString() const {
	stringstream ss;
	ss << "columns = { ";
	for (idx_t i = 0; i < Size(); ++i) {
		ss << SQLString::ToString(names->at(i).GetIdentifierName()) << " : ";
		ss << SQLString::ToString(types->at(i).ToString());
		if (i != Size() - 1) {
			ss << ", ";
		}
	}
	ss << "}";
	return ss.str();
}
} // namespace duckdb
