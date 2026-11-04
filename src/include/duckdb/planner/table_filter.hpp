//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/table_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/common/column_index.hpp"

namespace duckdb {
class BaseStatistics;
class Expression;
class PhysicalOperator;
class PhysicalTableScan;

enum class TableFilterType : uint8_t {
	LEGACY_CONSTANT_COMPARISON = 0,  // constant comparison (e.g. =C, >C, >=C, <C, <=C)
	LEGACY_IS_NULL = 1,              // C IS NULL
	LEGACY_IS_NOT_NULL = 2,          // C IS NOT NULL
	LEGACY_CONJUNCTION_OR = 3,       // OR of different filters
	LEGACY_CONJUNCTION_AND = 4,      // AND of different filters
	LEGACY_STRUCT_EXTRACT = 5,       // filter applies to child-column of struct
	LEGACY_OPTIONAL_FILTER = 6,      // executing filter is not required for query correctness
	LEGACY_IN_FILTER = 7,            // col IN (C1, C2, C3, ...)
	LEGACY_DYNAMIC_FILTER = 8,       // dynamic filters can be updated at run-time
	EXPRESSION_FILTER = 9,           // an arbitrary expression
	LEGACY_BLOOM_FILTER = 10,        // a probabilistic filter that can test whether a value is in a set of other value
	LEGACY_PREFIX_RANGE_FILTER = 12, // probabilistic range-based filter
};

//! TableFilter represents a filter pushed down into the table scan.
class TableFilter {
public:
	explicit TableFilter(TableFilterType filter_type_p) : filter_type(filter_type_p) {
	}
	virtual ~TableFilter() {
	}

	TableFilterType filter_type;

public:
	virtual unique_ptr<Expression> ToExpression(const Expression &column) const = 0;
	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<TableFilter> Deserialize(Deserializer &deserializer);

public:
	template <class TARGET>
	TARGET &Cast() {
		if (filter_type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast to type - table filter type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (filter_type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast to type - table filter type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
