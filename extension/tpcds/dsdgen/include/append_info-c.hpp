#pragma once

#include "duckdb/main/appender.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/storage/optimistic_data_writer.hpp"
#include "duckdb/storage/table/append_state.hpp"

#include <memory>
#include <cstdint>

namespace tpcds {

struct tpcds_table_def {
	const char *name;
	int fl_small;
	int fl_child;
	int first_column;
	int64_t *null_bitmap;
};

#define CALL_CENTER   0
#define DBGEN_VERSION 24

enum class TPCDSAppendType : uint8_t {
	INT32,
	INT64,
	VARCHAR,
	BOOLEAN,
	DATE,
	DECIMAL16,
	DECIMAL32,
	DECIMAL64,
	DECIMAL128
};

enum class TPCDSAppendMode : uint8_t { APPENDER, OPTIMISTIC };

struct tpcds_append_information {
	tpcds_append_information(duckdb::ClientContext &context_p, duckdb::TableCatalogEntry *table,
	                         TPCDSAppendMode mode = TPCDSAppendMode::APPENDER,
	                         duckdb::idx_t flush_count = duckdb::BaseAppender::DEFAULT_FLUSH_COUNT,
	                         duckdb::OptimisticWritePartialManagers partial_manager_type =
	                             duckdb::OptimisticWritePartialManagers::PER_COLUMN);

	duckdb::ClientContext &context;
	duckdb::unique_ptr<duckdb::InternalAppender> appender;
	duckdb::unique_ptr<duckdb::OptimisticDataWriter> optimistic_writer;
	duckdb::optional_ptr<duckdb::OptimisticWriteCollection> optimistic_collection;
	duckdb::optional_ptr<duckdb::DuckTableEntry> table_entry;
	duckdb::PhysicalIndex optimistic_collection_index = duckdb::PhysicalIndex(duckdb::DConstants::INVALID_INDEX);
	duckdb::TableAppendState append_state;
	duckdb::DataChunk chunk;
	duckdb::vector<duckdb::LogicalType> types;
	duckdb::vector<TPCDSAppendType> append_types;
	duckdb::vector<uint32_t> decimal_scales;
	duckdb::idx_t row = 0;
	duckdb::idx_t active_row = duckdb::DConstants::INVALID_INDEX;
	duckdb::idx_t active_col = 0;
	bool finalized = false;

	tpcds_table_def table_def;

	~tpcds_append_information();

	bool IsNull(int nColumn);
	void BeginRow();
	void EndRow();
	void FlushChunk();
	void FlushOptimistic();
	void FinalizeOptimisticAppend();
	void PrepareOptimisticWriteToDisk();
	void ResetOptimisticCollection();
	void Close();
	duckdb::Vector &NextColumn();
	TPCDSAppendType ActiveAppendType() const;
	uint32_t ActiveDecimalScale() const;
	void AppendNull();
	void AppendVarchar(const char *value);
	void AppendKey(int64_t value);
	void AppendInteger(int32_t value);
	void AppendIntegerDecimal(int32_t value);
	void AppendBoolean(int32_t value);
	void AppendDate(duckdb::date_t value);
	void AppendDecimal(int64_t value);
};

} // namespace tpcds
