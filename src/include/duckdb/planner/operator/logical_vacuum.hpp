//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_vacuum.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/parser/parsed_data/vacuum_info.hpp"

namespace duckdb {
class TableCatalogEntry;

//! LogicalVacuum represents a simple logical operator that only passes on the parse info
class LogicalVacuum : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_VACUUM;

public:
	LogicalVacuum();
	explicit LogicalVacuum(unique_ptr<VacuumInfo> info);

public:
	VacuumInfo &GetInfo() {
		return *info;
	}

	TableCatalogEntry &GetTable();
	bool HasTable() const;
	void SetTable(TableCatalogEntry &table_p);

public:
	optional_ptr<TableCatalogEntry> table;
	unordered_map<idx_t, idx_t> column_id_map;
	unique_ptr<VacuumInfo> info;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
	idx_t EstimateCardinality(ClientContext &context) override;

protected:
	void ResolveTypes() override {
		types.emplace_back(LogicalType::BOOLEAN);
	}
};

} // namespace duckdb
