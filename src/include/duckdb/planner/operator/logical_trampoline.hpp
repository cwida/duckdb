//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_trampoline.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

//! Trampoline-style iteration: children[0] is the seed, children[1..n] are the
//! branch plans. Rows are routed between branches by the selector column.
class LogicalTrampoline : public LogicalCTE {
	LogicalTrampoline();

public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_TRAMPOLINE;

public:
	LogicalTrampoline(Identifier ctename_p, TableIndex table_index, idx_t column_count, idx_t selector_idx,
	                  unique_ptr<LogicalOperator> seed, vector<unique_ptr<LogicalOperator>> branches);

	//! Index of the selector column in the trampoline schema
	idx_t selector_idx;
	//! Per-branch TEE flag: every row routed to the branch is also emitted
	vector<bool> branch_tee;
	//! Whether any branch reads frontier.<ctename>; decided at binding and kept
	//! through optimization so pruning a reference cannot change the round contract
	bool reads_frontier = false;
	//! Per-branch DISTINCT flag: round-wide admission against all rows ever routed
	vector<bool> branch_distinct;
	//! Per-branch MERGE flag: one bag wave per round, coalesced at the boundary
	vector<bool> branch_merge;
	//! Whether emitted rows are deduplicated against all rows ever emitted
	bool emit_distinct = false;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	vector<ColumnBinding> GetColumnBindings() override {
		return GenerateColumnBindings(table_index, column_count);
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

	idx_t BranchCount() const {
		return children.size() - 1;
	}

private:
	friend class LogicalPlanSQLExportContext;
	LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
	ExportSQLDefinition(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path,
	                    const Identifier &name);

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
