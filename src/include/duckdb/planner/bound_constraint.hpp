//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/bound_constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {
//! Bound equivalent of Constraint
class BoundConstraint {
public:
	explicit BoundConstraint(ConstraintType type) : type(type) {};
	virtual ~BoundConstraint() {
	}

	ConstraintType type;

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast constraint to type - bound constraint type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast constraint to type - bound constraint type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

	virtual unique_ptr<BoundConstraint> Copy() const = 0;
};
} // namespace duckdb
