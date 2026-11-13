//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_export.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <utility>

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/parsed_data/exported_table_data.hpp"
#include "duckdb/catalog/catalog_entry_map.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

struct ExportEntries {
	vector<reference<CatalogEntry>> schemas;
	vector<reference<CatalogEntry>> custom_types;
	vector<reference<CatalogEntry>> sequences;
	vector<reference<CatalogEntry>> tables;
	vector<reference<CatalogEntry>> views;
	vector<reference<CatalogEntry>> indexes;
	vector<reference<CatalogEntry>> macros;
	vector<reference<CatalogEntry>> triggers;
};

//! Parse a file from disk using a specified copy function and return the set of chunks retrieved from the file
class PhysicalExport : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXPORT;

public:
	PhysicalExport(PhysicalPlan &physical_plan, vector<LogicalType> types, CopyFunction function,
	               unique_ptr<CopyInfo> info, idx_t estimated_cardinality, unique_ptr<BoundExportData> exported_tables);

	//! The copy function to use to read the file
	CopyFunction function;
	//! The binding info containing the set of options for reading the file
	unique_ptr<CopyInfo> info;
	//! The table info for each table that will be exported
	unique_ptr<BoundExportData> exported_tables;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

	static void ExtractEntries(ClientContext &context, vector<reference<SchemaCatalogEntry>> &schemas,
	                           ExportEntries &result);
	static catalog_entry_vector_t GetNaiveExportOrder(ClientContext &context, Catalog &catalog);

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool ParallelSink() const override {
		return true;
	}
	bool IsSink() const override {
		return true;
	}

public:
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;
	vector<const_reference<PhysicalOperator>> GetSources() const override;
};

} // namespace duckdb
