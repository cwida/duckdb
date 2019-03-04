#include "capi_helpers.hpp"

#include <limits>

using namespace std;

int64_t NULL_NUMERIC = std::numeric_limits<int64_t>::min();
double NULL_DECIMAL = NAN;

template <class T> T get_numeric(duckdb_column column, size_t index) {
	T *data = (T *)column.data;
	return data[index];
}

int64_t get_numeric(duckdb_column column, size_t row) {
	switch (column.type) {
	case DUCKDB_TYPE_BOOLEAN:
	case DUCKDB_TYPE_TINYINT:
		return get_numeric<int8_t>(column, row);
	case DUCKDB_TYPE_SMALLINT:
		return get_numeric<int16_t>(column, row);
	case DUCKDB_TYPE_INTEGER:
		return get_numeric<int32_t>(column, row);
	case DUCKDB_TYPE_BIGINT:
		return get_numeric<int64_t>(column, row);
	default:
		return -1;
	}
}

bool CHECK_NUMERIC_COLUMN(duckdb_result result, size_t column, vector<int64_t> values) {
	if (result.column_count <= column) {
		// out of bounds
		return false;
	}
	auto &col = result.columns[column];
	if (col.type < DUCKDB_TYPE_BOOLEAN || col.type > DUCKDB_TYPE_BIGINT) {
		// not numeric type
		return false;
	}
	if (values.size() != col.count) {
		return false;
	}
	for (size_t i = 0; i < values.size(); i++) {
		if (values[i] == NULL_NUMERIC) {
			if (!duckdb_value_is_null(col, i)) {
				return false;
			}
		} else {
			int64_t data_value = get_numeric(col, i);
			if (data_value != values[i]) {
				return false;
			}
		}
	}
	return true;
}

bool CHECK_DECIMAL_COLUMN(duckdb_result result, size_t column, vector<double> values) {
	if (result.column_count <= column) {
		// out of bounds
		return false;
	}
	auto &col = result.columns[column];
	// not double type
	if (col.type != DUCKDB_TYPE_DECIMAL) {
		return false;
	}
	if (values.size() != col.count) {
		return false;
	}
	for (size_t i = 0; i < values.size(); i++) {
		if (std::isnan(values[i])) {
			if (!duckdb_value_is_null(col, i)) {
				return false;
			}
		} else {
			double right = values[i];
			double data_value = get_numeric<double>(col, i);
			if (abs(data_value - right) > 0.001) {
				return false;
			}
		}
	}
	return true;
}

bool CHECK_NUMERIC(duckdb_result result, size_t row, size_t column, int64_t value) {
	if (result.column_count <= column || result.row_count <= row) {
		// out of bounds
		return false;
	}
	auto &col = result.columns[column];
	if (col.type < DUCKDB_TYPE_BOOLEAN || col.type > DUCKDB_TYPE_BIGINT) {
		// not numeric type
		return false;
	}
	if (value == NULL_NUMERIC) {
		return duckdb_value_is_null(col, row);
	} else {
		int64_t data_value = get_numeric(col, row);
		return data_value == value;
	}
}

bool CHECK_STRING(duckdb_result result, size_t row, size_t column, string value) {
	if (result.column_count < column || result.row_count < row) {
		// out of bounds
		return false;
	}
	auto &col = result.columns[column];
	if (col.type != DUCKDB_TYPE_VARCHAR) {
		// not string type
		return false;
	}
	char **ptr = (char **)col.data;
	return string(ptr[row]) == value;
}

bool CHECK_STRING_COLUMN(duckdb_result result, size_t column, vector<string> values) {
	if (result.column_count <= column) {
		// out of bounds
		return false;
	}
	auto &col = result.columns[column];
	// not double type
	if (col.type != DUCKDB_TYPE_VARCHAR) {
		return false;
	}
	if (values.size() != col.count) {
		return false;
	}
	for (size_t i = 0; i < values.size(); i++) {
		if (!CHECK_STRING(result, i, column, values[i])) {
			return false;
		}
	}
	return true;
}
