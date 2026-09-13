#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

SelectStatement::SelectStatement(const SelectStatement &other) : SQLStatement(other), node(other.node->Copy()) {
	for (auto &selector : other.at_clause_subqueries) {
		at_clause_subqueries.push_back(unique_ptr_cast<SQLStatement, SelectStatement>(selector->Copy()));
	}
}

unique_ptr<SQLStatement> SelectStatement::Copy() const {
	return unique_ptr<SelectStatement>(new SelectStatement(*this));
}

bool SelectStatement::Equals(const SQLStatement &other_p) const {
	if (type != other_p.type) {
		return false;
	}
	auto &other = other_p.Cast<SelectStatement>();
	return node->Equals(other.node.get());
}

string SelectStatement::ToString() const {
	return node->ToString();
}

} // namespace duckdb
