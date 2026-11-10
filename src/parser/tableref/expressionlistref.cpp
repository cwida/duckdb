#include "duckdb/parser/tableref/expressionlistref.hpp"

namespace duckdb {

string ExpressionListRef::ToString() const {
	return BaseToString(ValuesToString(), expected_names);
}

string ExpressionListRef::ValuesToString() const {
	D_ASSERT(!values.empty());
	string result = "(VALUES ";
	for (idx_t row_idx = 0; row_idx < values.size(); row_idx++) {
		if (row_idx > 0) {
			result += ", ";
		}
		auto &row = values[row_idx];
		result += "(";
		for (idx_t col_idx = 0; col_idx < row.size(); col_idx++) {
			if (col_idx > 0) {
				result += ", ";
			}
			result += row[col_idx]->ToString();
		}
		result += ")";
	}
	result += ")";
	return result;
}

bool ExpressionListRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<ExpressionListRef>();
	if (values.size() != other.values.size()) {
		return false;
	}
	for (idx_t i = 0; i < values.size(); i++) {
		if (values[i].size() != other.values[i].size()) {
			return false;
		}
		for (idx_t j = 0; j < values[i].size(); j++) {
			if (!values[i][j]->Equals(*other.values[i][j])) {
				return false;
			}
		}
	}
	return true;
}

unique_ptr<TableRef> ExpressionListRef::Copy() {
	// value list
	auto result = make_uniq<ExpressionListRef>();
	for (auto &val_list : values) {
		vector<unique_ptr<ParsedExpression>> new_val_list;
		new_val_list.reserve(val_list.size());
		for (auto &val : val_list) {
			new_val_list.push_back(val->Copy());
		}
		result->values.push_back(std::move(new_val_list));
	}
	result->expected_names = expected_names;
	result->expected_types = expected_types;
	CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
