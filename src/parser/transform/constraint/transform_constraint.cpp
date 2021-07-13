#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<Constraint> Transformer::TransformConstraint(duckdb_libpgquery::PGListCell *cell) {
	auto constraint = reinterpret_cast<duckdb_libpgquery::PGConstraint *>(cell->data.ptr_value);
	switch (constraint->contype) {
	case duckdb_libpgquery::PG_CONSTR_UNIQUE:
	case duckdb_libpgquery::PG_CONSTR_PRIMARY: {
		bool is_primary_key = constraint->contype == duckdb_libpgquery::PG_CONSTR_PRIMARY;
		vector<string> columns;
		for (auto kc = constraint->keys->head; kc; kc = kc->next) {
			columns.emplace_back(reinterpret_cast<duckdb_libpgquery::PGValue *>(kc->data.ptr_value)->val.str);
		}
		return make_unique<UniqueConstraint>(columns, is_primary_key);
	}
	case duckdb_libpgquery::PG_CONSTR_CHECK: {
		auto expression = TransformExpression(constraint->raw_expr, 0);
		if (expression->HasSubquery()) {
			throw ParserException("subqueries prohibited in CHECK constraints");
		}
		return make_unique<CheckConstraint>(TransformExpression(constraint->raw_expr, 0));
	}
	default:
		throw NotImplementedException("Constraint type not handled yet!");
	}
}

unique_ptr<Constraint> Transformer::TransformConstraint(duckdb_libpgquery::PGListCell *cell, ColumnDefinition &column,
                                                        idx_t index) {
	auto constraint = reinterpret_cast<duckdb_libpgquery::PGConstraint *>(cell->data.ptr_value);
	D_ASSERT(constraint);
	switch (constraint->contype) {
	case duckdb_libpgquery::PG_CONSTR_NOTNULL:
		return make_unique<NotNullConstraint>(index);
	case duckdb_libpgquery::PG_CONSTR_CHECK:
		return TransformConstraint(cell);
	case duckdb_libpgquery::PG_CONSTR_PRIMARY:
		return make_unique<UniqueConstraint>(index, true);
	case duckdb_libpgquery::PG_CONSTR_UNIQUE:
		return make_unique<UniqueConstraint>(index, false);
	case duckdb_libpgquery::PG_CONSTR_NULL:
		return nullptr;
	case duckdb_libpgquery::PG_CONSTR_DEFAULT:
		column.default_value = TransformExpression(constraint->raw_expr, 0);
		return nullptr;
	case duckdb_libpgquery::PG_CONSTR_FOREIGN:
	default:
		throw NotImplementedException("Constraint not implemented!");
	}
}

} // namespace duckdb
