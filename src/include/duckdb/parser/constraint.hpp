//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

//===--------------------------------------------------------------------===//
// Constraint Types
//===--------------------------------------------------------------------===//
enum class ConstraintType : uint8_t {
	INVALID = 0,     // invalid constraint type
	NOT_NULL = 1,    // NOT NULL constraint
	CHECK = 2,       // CHECK constraint
	UNIQUE = 3,      // UNIQUE constraint
	FOREIGN_KEY = 4, // FOREIGN KEY constraint
};

enum class ForeignKeyType : uint8_t {
	FK_TYPE_PRIMARY_KEY_TABLE = 0,   // main table
	FK_TYPE_FOREIGN_KEY_TABLE = 1,   // referencing table
	FK_TYPE_SELF_REFERENCE_TABLE = 2 // self referencing table
};

struct ForeignKeyInfo {
	ForeignKeyType type;
	Identifier schema;
	//! if type is FK_TYPE_FOREIGN_KEY_TABLE, means main key table, if type is FK_TYPE_PRIMARY_KEY_TABLE, means foreign
	//! key table
	Identifier table;
	//! The set of main key table's column's index
	vector<PhysicalIndex> pk_keys;
	//! The set of foreign key table's column's index
	vector<PhysicalIndex> fk_keys;

	bool IsDeleteConstraint() const {
		return type == ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE ||
		       type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE;
	}
	bool IsAppendConstraint() const {
		return type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE ||
		       type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE;
	}
};

//! Constraint is the base class of any type of table constraint.
class Constraint {
public:
	DUCKDB_API explicit Constraint(ConstraintType type);
	DUCKDB_API virtual ~Constraint();

	ConstraintType type;

public:
	DUCKDB_API virtual string ToString() const = 0;
	DUCKDB_API void Print() const;

	DUCKDB_API virtual unique_ptr<Constraint> Copy() const = 0;

	DUCKDB_API virtual void Serialize(Serializer &serializer) const;
	DUCKDB_API static unique_ptr<Constraint> Deserialize(Deserializer &deserializer);

	idx_t GetBackingIndexOid() const;
	void SetBackingIndexOid(idx_t oid);

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast constraint to type - constraint type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast constraint to type - constraint type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

private:
	//! Whether the table enforces this constraint through an index it owns.
	bool NeedsBackingIndex() const;

	//! The backing index identity is local to this database instance and is never persisted.
	optional_idx backing_index_oid;
};
} // namespace duckdb
