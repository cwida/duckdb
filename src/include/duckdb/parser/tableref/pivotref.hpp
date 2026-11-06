//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/pivotref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"

namespace duckdb {

struct PivotColumnEntry {
	//! The set of values to match on (PIVOT only)
	vector<Value> values;
	//! The expression (UNPIVOT only)
	unique_ptr<ParsedExpression> expr;
	//! The alias of the pivot column entry
	Identifier alias;

	bool Equals(const PivotColumnEntry &other) const;
	PivotColumnEntry Copy() const;

	//! Whether an UNPIVOT entry expression is a list of source column names (unqualified columns, constants, or a row
	//! of them) rather than an expression to unpivot; appends the names when it is
	static bool TryExtractUnpivotColumnList(const ParsedExpression &expr, vector<Identifier> &column_names);

	void Serialize(Serializer &serializer) const;
	static PivotColumnEntry Deserialize(Deserializer &source);
};

struct PivotValueElement {
	vector<Value> values;
	string name;
};

struct PivotColumn {
	//! The set of expressions to pivot on
	vector<unique_ptr<ParsedExpression>> pivot_expressions;
	//! The set of unpivot names
	vector<Identifier> unpivot_names;
	//! The set of values to pivot on
	vector<PivotColumnEntry> entries;
	//! The enum to read pivot values from (if any)
	Identifier pivot_enum;
	//! Subquery (if any) - used during transform only
	unique_ptr<QueryNode> subquery;

	string ToString() const;
	bool Equals(const PivotColumn &other) const;
	PivotColumn Copy() const;

	void Serialize(Serializer &serializer) const;
	static PivotColumn Deserialize(Deserializer &source);

	vector<PivotColumnEntry> GetEntriesForSerialization(Serializer &serializer) const;
};

//! Represents a PIVOT or UNPIVOT expression
class PivotRef : public TableRef {
public:
	static constexpr const TableReferenceType TYPE = TableReferenceType::PIVOT;

public:
	explicit PivotRef() : TableRef(TableReferenceType::PIVOT), include_nulls(false) {
	}

	//! The source table of the pivot
	unique_ptr<TableRef> source;
	//! The aggregates to compute over the pivot (PIVOT only)
	vector<unique_ptr<ParsedExpression>> aggregates;
	//! The names of the unpivot expressions (UNPIVOT only)
	vector<Identifier> unpivot_names;
	//! The set of pivots
	vector<PivotColumn> pivots;
	//! The groups to pivot over. If none are specified all columns not included in the pivots/aggregate are chosen.
	vector<Identifier> groups;
	//! Whether or not to include nulls in the result (UNPIVOT only)
	bool include_nulls;
	//! The set of values to pivot on (bound pivot only)
	vector<PivotValueElement> bound_pivot_values;
	//! The set of bound group names (bound pivot only)
	vector<Identifier> bound_group_names;
	//! The set of bound aggregate names (bound pivot only)
	vector<Identifier> bound_aggregate_names;

public:
	string ToString() const override;
	bool Equals(const TableRef &other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Deserializes a blob back into a PivotRef
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableRef> Deserialize(Deserializer &source);
};
} // namespace duckdb
