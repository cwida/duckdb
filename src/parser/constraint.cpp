#include "duckdb/parser/constraint.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"

namespace duckdb {

Constraint::Constraint(ConstraintType type) : type(type) {
}

Constraint::~Constraint() {
}

void Constraint::Print() const {
	Printer::Print(ToString());
}

bool Constraint::NeedsBackingIndex() const {
	switch (type) {
	case ConstraintType::UNIQUE:
		return true;
	case ConstraintType::FOREIGN_KEY:
		// Reverse foreign key constraints on the referenced table do not own a backing index.
		return Cast<ForeignKeyConstraint>().info.IsAppendConstraint();
	default:
		return false;
	}
}

idx_t Constraint::GetBackingIndexOid() const {
	return backing_index_oid.GetIndex();
}

void Constraint::SetBackingIndexOid(idx_t oid) {
	D_ASSERT(NeedsBackingIndex());
	D_ASSERT(oid != DConstants::INVALID_INDEX);
	backing_index_oid = oid;
}

} // namespace duckdb
