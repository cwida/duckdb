#include "duckdb/parser/tableref/emptytableref.hpp"

namespace duckdb {

string EmptyTableRef::ToString() const {
	return "";
}

bool EmptyTableRef::Equals(const TableRef &other) const {
	return TableRef::Equals(other);
}

unique_ptr<TableRef> EmptyTableRef::Copy() {
	auto copy = make_uniq<EmptyTableRef>();
	CopyProperties(*copy);
	return std::move(copy);
}

} // namespace duckdb
