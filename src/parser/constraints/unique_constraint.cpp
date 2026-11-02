#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/index_constraint_type.hpp"

namespace duckdb {

UniqueConstraint::UniqueConstraint()
    : Constraint(ConstraintType::UNIQUE), index(DConstants::INVALID_INDEX), is_primary_key(false),
      check_mode(ConstraintCheckMode::DEFAULT) {
}

UniqueConstraint::UniqueConstraint(const LogicalIndex index, const bool is_primary_key, ConstraintCheckMode check_mode)
    : Constraint(ConstraintType::UNIQUE), index(index), is_primary_key(is_primary_key), check_mode(check_mode) {
}

UniqueConstraint::UniqueConstraint(const LogicalIndex index, Identifier column_name_p, const bool is_primary_key,
                                   ConstraintCheckMode check_mode)
    : UniqueConstraint(index, is_primary_key, check_mode) {
	columns.emplace_back(std::move(column_name_p));
}

UniqueConstraint::UniqueConstraint(vector<Identifier> columns, const bool is_primary_key,
                                   ConstraintCheckMode check_mode)
    : Constraint(ConstraintType::UNIQUE), index(DConstants::INVALID_INDEX), columns(std::move(columns)),
      is_primary_key(is_primary_key), check_mode(check_mode) {
}

string UniqueConstraint::ToString() const {
	string base = is_primary_key ? "PRIMARY KEY(" : "UNIQUE(";
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			base += ", ";
		}
		base += SQLIdentifier(columns[i]);
	}
	base += ")";
	if (check_mode != ConstraintCheckMode::DEFAULT) {
		base += " " + EnumUtil::ToString(check_mode);
	}
	return base;
}

unique_ptr<Constraint> UniqueConstraint::Copy() const {
	return make_uniq<UniqueConstraint>(*this);
}

bool UniqueConstraint::IsPrimaryKey() const {
	return is_primary_key;
}

bool UniqueConstraint::IsDeferred() const {
	return check_mode == ConstraintCheckMode::DEFERRED;
}

IndexConstraintType UniqueConstraint::GetIndexConstraintType() const {
	return IsPrimaryKey() ? IndexConstraintType::PRIMARY : IndexConstraintType::UNIQUE;
}

bool UniqueConstraint::HasIndex() const {
	return index.index != DConstants::INVALID_INDEX;
}

LogicalIndex UniqueConstraint::GetIndex() const {
	if (!HasIndex()) {
		throw InternalException("UniqueConstraint::GetIndex called on a unique constraint without an index");
	}
	return index;
}

void UniqueConstraint::SetIndex(const LogicalIndex new_index) {
	D_ASSERT(new_index.index != DConstants::INVALID_INDEX);
	index = new_index;
}

const vector<Identifier> &UniqueConstraint::GetColumnNames() const {
	D_ASSERT(!columns.empty());
	return columns;
}

vector<Identifier> &UniqueConstraint::GetColumnNamesMutable() {
	D_ASSERT(!columns.empty());
	return columns;
}

vector<LogicalIndex> UniqueConstraint::GetLogicalIndexes(const ColumnList &column_list) const {
	if (HasIndex()) {
		return {GetIndex()};
	}

	vector<LogicalIndex> indexes;
	for (auto &col_name : GetColumnNames()) {
		D_ASSERT(column_list.ColumnExists(col_name));
		auto &col = column_list.GetColumn(col_name);
		D_ASSERT(!col.Generated());
		indexes.push_back(col.Logical());
	}
	return indexes;
}

Identifier UniqueConstraint::GetName(const Identifier &table_name) const {
	auto type_name = EnumUtil::ToString(GetIndexConstraintType());

	string name;
	for (const auto &column_name : GetColumnNames()) {
		name += "_" + column_name;
	}
	return Identifier(type_name + "_" + table_name + name);
}

} // namespace duckdb
