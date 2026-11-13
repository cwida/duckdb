#include "duckdb/parser/tableref/at_clause.hpp"

namespace duckdb {

AtClause::AtClause(Identifier unit_p, unique_ptr<ParsedExpression> expr_p)
    : unit(std::move(unit_p)), expr(std::move(expr_p)) {
}

string AtClause::ToString() const {
	return "AT (" + unit + " => " + expr->ToString() + ")";
}

bool AtClause::Equals(const AtClause &other) const {
	return unit == other.unit && expr->Equals(*other.expr);
}

unique_ptr<AtClause> AtClause::Copy() const {
	auto result = make_uniq<AtClause>(unit, expr->Copy());
	result->prebind_index = prebind_index;
	return result;
}

bool AtClause::Equals(optional_ptr<AtClause> lhs, optional_ptr<AtClause> rhs) {
	if (lhs.get() == rhs.get()) {
		// same pointer OR both are null
		return true;
	}
	if (!lhs || !rhs) {
		// one is NULL other is not - always not equal
		return false;
	}
	return lhs->Equals(*rhs);
}

} // namespace duckdb
