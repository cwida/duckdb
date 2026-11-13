#include "duckdb/parser/tableref/subqueryref.hpp"

#include "duckdb/common/limits.hpp"

namespace duckdb {

string SubqueryRef::ToString() const {
	string result = "(" + subquery->ToString() + ")";
	return BaseToString(result, column_name_alias);
}

SubqueryRef::SubqueryRef() : TableRef(TableReferenceType::SUBQUERY) {
}

SubqueryRef::SubqueryRef(unique_ptr<SelectStatement> subquery_p, Identifier alias_p)
    : TableRef(TableReferenceType::SUBQUERY), subquery(std::move(subquery_p)) {
	this->alias = std::move(alias_p);
}

bool SubqueryRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<SubqueryRef>();
	return subquery->Equals(*other.subquery);
}

unique_ptr<TableRef> SubqueryRef::Copy() {
	auto copy = make_uniq<SubqueryRef>(unique_ptr_cast<SQLStatement, SelectStatement>(subquery->Copy()), alias);
	copy->column_name_alias = column_name_alias;
	copy->deduplicate_column_names = deduplicate_column_names;
	CopyProperties(*copy);
	return std::move(copy);
}

} // namespace duckdb
