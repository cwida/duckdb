//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_materialized_cte.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalCTERef;

//! Retaining outer rows preserves every domain partition needed for their results.
//! The domain scan has no consumers independent of the outer-row scan.
struct CTEFilterDependency {
	CTEFilterDependency(TableIndex row_scan, TableIndex domain_scan) : row_scan(row_scan), domain_scan(domain_scan) {
	}

	bool MatchesConsumers(const vector<reference<LogicalCTERef>> &references) const;

	TableIndex row_scan;
	TableIndex domain_scan;
};

class LogicalMaterializedCTE : public LogicalCTE {
	explicit LogicalMaterializedCTE() : LogicalCTE(LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
	}

public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;

public:
	LogicalMaterializedCTE(Identifier ctename_p, TableIndex table_index, idx_t column_count,
	                       unique_ptr<LogicalOperator> cte, unique_ptr<LogicalOperator> child,
	                       CTEMaterialize materialize)
	    : LogicalCTE(std::move(ctename_p), table_index, column_count, std::move(cte), std::move(child),
	                 LogicalOperatorType::LOGICAL_MATERIALIZED_CTE),
	      materialize(materialize) {
	}

	CTEMaterialize materialize = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	//! Valid only for the original generated consumers, until CTE filter pushdown. Deliberately not serialized.
	unique_ptr<CTEFilterDependency> filter_dependency;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;
	vector<ColumnBinding> GetColumnBindings() override {
		return children[1]->GetColumnBindings();
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	vector<TableIndex> GetTableIndex() const override;

protected:
	void ResolveTypes() override {
		types = children[1]->types;
	}
};
} // namespace duckdb
