//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/constraints/unique_constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/enums/constraint_check_mode.hpp"
#include "duckdb/common/enums/index_constraint_type.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/constraint.hpp"

namespace duckdb {

class UniqueConstraint : public Constraint {
public:
	static constexpr const ConstraintType TYPE = ConstraintType::UNIQUE;

public:
	DUCKDB_API UniqueConstraint(const LogicalIndex index, const bool is_primary_key,
	                            ConstraintCheckMode check_mode = ConstraintCheckMode::DEFAULT);
	DUCKDB_API UniqueConstraint(const LogicalIndex index, Identifier column_name, const bool is_primary_key,
	                            ConstraintCheckMode check_mode = ConstraintCheckMode::DEFAULT);
	DUCKDB_API UniqueConstraint(vector<Identifier> columns, const bool is_primary_key,
	                            ConstraintCheckMode check_mode = ConstraintCheckMode::DEFAULT);

public:
	DUCKDB_API string ToString() const override;
	DUCKDB_API unique_ptr<Constraint> Copy() const override;
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<Constraint> Deserialize(Deserializer &deserializer);

	//! Returns true, if the constraint is a PRIMARY KEY constraint.
	bool IsPrimaryKey() const;
	//! Returns true if the constraint was declared with the DEFERRED modifier.
	bool IsDeferred() const;
	//! Returns the index constraint type used to enforce this constraint.
	IndexConstraintType GetIndexConstraintType() const;
	//! Returns true, if the constraint is defined on a single column.
	bool HasIndex() const;
	//! Returns the column index on which the constraint is defined.
	LogicalIndex GetIndex() const;
	//! Sets the column index of the constraint.
	void SetIndex(const LogicalIndex new_index);
	//! Returns a constant reference to the column names on which the constraint is defined.
	const vector<Identifier> &GetColumnNames() const;
	//! Returns a mutable reference to the column names on which the constraint is defined.
	vector<Identifier> &GetColumnNamesMutable();
	//! Returns the column indexes on which the constraint is defined.
	vector<LogicalIndex> GetLogicalIndexes(const ColumnList &columns) const;
	//! Get the name of the constraint.
	Identifier GetName(const Identifier &table_name) const;

private:
	UniqueConstraint();

#ifdef DUCKDB_API_1_0
private:
#else
public:
#endif

	//! The indexed column of the constraint. Only used for single-column constraints, invalid otherwise.
	LogicalIndex index;
	//! The names of the columns on which this constraint is defined. Only set if the index field is not set.
	vector<Identifier> columns;
	//! Whether this is a PRIMARY KEY constraint, or a UNIQUE constraint.
	bool is_primary_key;
	//! The declared constraint check mode, or DEFAULT when no modifier was specified.
	ConstraintCheckMode check_mode;
};

} // namespace duckdb
