#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

ShowRef::ShowRef() : TableRef(TableReferenceType::SHOW_REF), show_type(ShowType::DESCRIBE) {
}

string ShowRef::ToString() const {
	string result;
	if (show_type == ShowType::SHOW) {
		result += "SHOW ";
		if (!GetTableName().empty()) {
			// "SHOW name" / "SHOW schema.table" keeps a describe fallback query, but its canonical form is the name
			result += qualified_name.ToString();
		} else if (query) {
			// "SHOW (SELECT ...)"
			result += "(" + query->ToString() + ")";
		}
		return result;
	}
	if (show_type == ShowType::SUMMARY) {
		result += "SUMMARIZE ";
	} else if (show_type == ShowType::SHOW_FROM) {
		result += "SHOW TABLES FROM " + qualified_name.Parent().ToString();
	} else {
		result += "DESCRIBE ";
	}
	if (query) {
		result += "(";
		result += query->ToString();
		result += ")";
	} else if (show_type == ShowType::SHOW_SPECIAL) {
		// The special-form names are stored pre-quoted (e.g. "\"databases\"") so the binder can recognize them;
		// emit them as-is so re-parsing yields the special form again
		if (GetTableName() != "__show_tables_expanded") {
			result += GetTableName().GetIdentifierName();
		}
	} else if (!GetTableName().empty()) {
		result += qualified_name.ToString();
	}
	return result;
}

bool ShowRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<ShowRef>();
	if (other.query.get() != query.get()) {
		if (!query || !query->Equals(other.query.get())) {
			return false;
		}
	}
	return qualified_name == other.qualified_name && show_type == other.show_type;
}

unique_ptr<TableRef> ShowRef::Copy() {
	auto copy = make_uniq<ShowRef>();

	copy->qualified_name = qualified_name;
	copy->query = query ? query->Copy() : nullptr;
	copy->show_type = show_type;
	CopyProperties(*copy);

	return std::move(copy);
}

} // namespace duckdb
