#include "append_info-c.hpp"

#include "append_info.h"
#include "config.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "porting.h"

#include <cstring>
#include <memory>
#include <string>
#include "duckdb/common/unique_ptr.hpp"

using namespace tpcds;

tpcds_append_information::tpcds_append_information(duckdb::ClientContext &context_p, duckdb::TableCatalogEntry *table,
                                                   TPCDSAppendMode mode, duckdb::idx_t flush_count,
                                                   duckdb::OptimisticWritePartialManagers partial_manager_type)
    : context(context_p), types(table->GetTypes()) {
	if (mode == TPCDSAppendMode::APPENDER) {
		appender = duckdb::make_uniq<duckdb::InternalAppender>(context_p, *table, flush_count);
	} else {
		table_entry = table->TryGetDuckTableEntry();
		if (!table_entry) {
			throw duckdb::InvalidInputException("dsdgen is only supported for DuckDB database files");
		}
		optimistic_writer = duckdb::make_uniq<duckdb::OptimisticDataWriter>(context_p, table_entry->GetStorage());
		auto collection = optimistic_writer->CreateCollection(table_entry->GetStorage(), types, partial_manager_type);
		auto &row_collection = *collection->collection;
		row_collection.InitializeEmpty();
		// the transaction id keeps the rows invisible to other transactions until the commit
		row_collection.InitializeAppend(duckdb::DuckTransaction::Get(context, table_entry->catalog), append_state);
		optimistic_collection_index =
		    table_entry->GetStorage().CreateOptimisticCollection(context, std::move(collection));
		optimistic_collection = table_entry->GetStorage().GetOptimisticCollection(context, optimistic_collection_index);
	}
	append_types.reserve(types.size());
	decimal_scales.reserve(types.size());
	for (auto &type : types) {
		decimal_scales.push_back(0);
		switch (type.id()) {
		case duckdb::LogicalTypeId::INTEGER:
			append_types.push_back(TPCDSAppendType::INT32);
			break;
		case duckdb::LogicalTypeId::BIGINT:
			append_types.push_back(TPCDSAppendType::INT64);
			break;
		case duckdb::LogicalTypeId::VARCHAR:
			append_types.push_back(TPCDSAppendType::VARCHAR);
			break;
		case duckdb::LogicalTypeId::BOOLEAN:
			append_types.push_back(TPCDSAppendType::BOOLEAN);
			break;
		case duckdb::LogicalTypeId::DATE:
			append_types.push_back(TPCDSAppendType::DATE);
			break;
		case duckdb::LogicalTypeId::DECIMAL:
			decimal_scales.back() = static_cast<uint8_t>(duckdb::DecimalType::GetScale(type));
			switch (type.InternalType()) {
			case duckdb::PhysicalType::INT16:
				append_types.push_back(TPCDSAppendType::DECIMAL16);
				break;
			case duckdb::PhysicalType::INT32:
				append_types.push_back(TPCDSAppendType::DECIMAL32);
				break;
			case duckdb::PhysicalType::INT64:
				append_types.push_back(TPCDSAppendType::DECIMAL64);
				break;
			case duckdb::PhysicalType::INT128:
				append_types.push_back(TPCDSAppendType::DECIMAL128);
				break;
			default:
				throw duckdb::InternalException("Unexpected TPC-DS decimal column type");
			}
			break;
		default:
			throw duckdb::InternalException("Unexpected TPC-DS column type");
		}
	}
	chunk.Initialize(context, types);
}

tpcds_append_information::~tpcds_append_information() {
	if (optimistic_writer) {
		optimistic_writer->Rollback();
	}
}

void tpcds_append_information::ResetOptimisticCollection() {
	D_ASSERT(table_entry);
	D_ASSERT(optimistic_collection_index.IsValid());
	table_entry->GetStorage().ResetOptimisticCollection(context, optimistic_collection_index);
	optimistic_collection_index = duckdb::PhysicalIndex(duckdb::DConstants::INVALID_INDEX);
	optimistic_collection = nullptr;
}

append_info *append_info_get(void *info_list, int table_id) {
	auto &append_vector = *((duckdb::vector<duckdb::unique_ptr<tpcds_append_information>> *)info_list);
	return (append_info *)append_vector[table_id].get();
}

bool tpcds_append_information::IsNull(int nColumn) {
	auto column_offset = nColumn - table_def.first_column;
	D_ASSERT(column_offset >= 0);
	auto bit_mask = int64_t(1) << column_offset;
	return ((*table_def.null_bitmap & bit_mask) != 0);
}

void tpcds_append_information::BeginRow() {
	D_ASSERT(appender || optimistic_collection);
	D_ASSERT(active_row == duckdb::DConstants::INVALID_INDEX);
	if (row >= STANDARD_VECTOR_SIZE) {
		FlushChunk();
	}
	active_row = row;
	active_col = 0;
}

void tpcds_append_information::EndRow() {
	D_ASSERT(active_row != duckdb::DConstants::INVALID_INDEX);
	D_ASSERT(active_col == chunk.ColumnCount());
	row++;
	active_row = duckdb::DConstants::INVALID_INDEX;
}

void tpcds_append_information::FlushChunk() {
	if (row == 0) {
		return;
	}
	D_ASSERT(active_row == duckdb::DConstants::INVALID_INDEX);
	chunk.SetChildCardinality(row);
	if (appender) {
		appender->AppendDataChunk(chunk);
	} else {
		D_ASSERT(optimistic_writer);
		D_ASSERT(optimistic_collection);
		auto &row_collection = *optimistic_collection->collection;
		auto flushed_row_group_idx = row_collection.Append(chunk, append_state);
		if (flushed_row_group_idx.IsValid()) {
			optimistic_writer->WriteNewRowGroup(*optimistic_collection, flushed_row_group_idx.GetIndex());
		}
	}
	chunk.Reset();
	row = 0;
	active_col = 0;
}

void tpcds_append_information::FinalizeOptimisticAppend() {
	D_ASSERT(optimistic_collection);
	if (finalized) {
		return;
	}
	FlushChunk();
	auto &row_collection = *optimistic_collection->collection;
	row_collection.FinalizeAppend(append_state.transaction, append_state);
	finalized = true;
}

void tpcds_append_information::PrepareOptimisticWriteToDisk() {
	if (!optimistic_collection) {
		return;
	}
	D_ASSERT(table_entry);
	D_ASSERT(optimistic_writer);
	FinalizeOptimisticAppend();
	auto &storage = table_entry->GetStorage();
	auto &row_collection = *optimistic_collection->collection;
	if (row_collection.GetTotalRows() >= storage.GetRowGroupSize()) {
		optimistic_writer->WriteUnflushedRowGroups(*optimistic_collection);
		optimistic_writer->FinalFlush();
	}
}

void tpcds_append_information::FlushOptimistic() {
	D_ASSERT(table_entry);
	D_ASSERT(optimistic_writer);
	D_ASSERT(optimistic_collection);
	FinalizeOptimisticAppend();

	auto &row_collection = *optimistic_collection->collection;
	auto append_count = row_collection.GetTotalRows();
	if (append_count == 0) {
		ResetOptimisticCollection();
		optimistic_writer.reset();
		finalized = false;
		return;
	}

	auto &table = *table_entry;
	auto &storage = table.GetStorage();
	if (append_count < storage.GetRowGroupSize()) {
		auto binder = duckdb::Binder::CreateBinder(context);
		auto bound_constraints = binder->BindConstraints(table);
		duckdb::LocalAppendState local_append_state;
		storage.InitializeLocalAppend(local_append_state, table, context, bound_constraints);
		auto &transaction = duckdb::DuckTransaction::Get(context, table.catalog);
		for (auto &insert_chunk : row_collection.Chunks(transaction)) {
			storage.LocalAppend(local_append_state, table, context, insert_chunk, false);
		}
		storage.FinalizeLocalAppend(local_append_state);
		ResetOptimisticCollection();
	} else {
		optimistic_writer->WriteUnflushedRowGroups(*optimistic_collection);
		storage.LocalMerge(context, table, *optimistic_collection);
		auto &storage_writer = storage.GetOptimisticWriter(context);
		storage_writer.Merge(*optimistic_writer);
		ResetOptimisticCollection();
	}
	optimistic_writer.reset();
	finalized = false;
}

void tpcds_append_information::Close() {
	FlushChunk();
	if (appender) {
		appender->Close();
		appender.reset();
	} else if (optimistic_collection) {
		FlushOptimistic();
	}
}

duckdb::Vector &tpcds_append_information::NextColumn() {
	D_ASSERT(active_row != duckdb::DConstants::INVALID_INDEX);
	D_ASSERT(active_col < chunk.ColumnCount());
	return chunk.data[active_col++];
}

TPCDSAppendType tpcds_append_information::ActiveAppendType() const {
	D_ASSERT(active_col < append_types.size());
	return append_types[active_col];
}

uint8_t tpcds_append_information::ActiveDecimalScale() const {
	D_ASSERT(active_col < decimal_scales.size());
	return decimal_scales[active_col];
}

void tpcds_append_information::AppendNull() {
	auto &vector = NextColumn();
	duckdb::FlatVector::SetNull(vector, active_row, true);
}

void tpcds_append_information::AppendVarchar(const char *value) {
	D_ASSERT(ActiveAppendType() == TPCDSAppendType::VARCHAR);
	auto &vector = NextColumn();
	duckdb::FlatVector::GetDataMutable<duckdb::string_t>(vector)[active_row] =
	    duckdb::StringVector::AddString(vector, value);
}

void tpcds_append_information::AppendKey(int64_t value) {
	auto append_type = ActiveAppendType();
	auto &vector = NextColumn();
	switch (append_type) {
	case TPCDSAppendType::INT64:
		duckdb::FlatVector::GetDataMutable<int64_t>(vector)[active_row] = value;
		break;
	case TPCDSAppendType::INT32:
		duckdb::FlatVector::GetDataMutable<int32_t>(vector)[active_row] = static_cast<int32_t>(value);
		break;
	case TPCDSAppendType::VARCHAR: {
		auto value_string = std::to_string(value);
		duckdb::FlatVector::GetDataMutable<duckdb::string_t>(vector)[active_row] =
		    duckdb::StringVector::AddString(vector, value_string);
		break;
	}
	default:
		throw duckdb::InternalException("Unexpected TPC-DS key column type");
	}
}

void tpcds_append_information::AppendInteger(int32_t value) {
	auto append_type = ActiveAppendType();
	auto &vector = NextColumn();
	switch (append_type) {
	case TPCDSAppendType::INT64:
		duckdb::FlatVector::GetDataMutable<int64_t>(vector)[active_row] = value;
		break;
	case TPCDSAppendType::INT32:
		duckdb::FlatVector::GetDataMutable<int32_t>(vector)[active_row] = value;
		break;
	case TPCDSAppendType::VARCHAR: {
		auto value_string = std::to_string(value);
		duckdb::FlatVector::GetDataMutable<duckdb::string_t>(vector)[active_row] =
		    duckdb::StringVector::AddString(vector, value_string);
		break;
	}
	default:
		throw duckdb::InternalException("Unexpected TPC-DS integer column type");
	}
}

void tpcds_append_information::AppendIntegerDecimal(int32_t value) {
	AppendDecimal(static_cast<int64_t>(value) * 100);
}

void tpcds_append_information::AppendBoolean(int32_t value) {
	D_ASSERT(ActiveAppendType() == TPCDSAppendType::BOOLEAN);
	auto &vector = NextColumn();
	duckdb::FlatVector::GetDataMutable<bool>(vector)[active_row] = value != 0;
}

void tpcds_append_information::AppendDate(duckdb::date_t value) {
	D_ASSERT(ActiveAppendType() == TPCDSAppendType::DATE);
	auto &vector = NextColumn();
	duckdb::FlatVector::GetDataMutable<duckdb::date_t>(vector)[active_row] = value;
}

void tpcds_append_information::AppendDecimal(int64_t value) {
	auto append_type = ActiveAppendType();
	auto &vector = NextColumn();
	switch (append_type) {
	case TPCDSAppendType::DECIMAL16:
		duckdb::FlatVector::GetDataMutable<int16_t>(vector)[active_row] = static_cast<int16_t>(value);
		break;
	case TPCDSAppendType::DECIMAL32:
		duckdb::FlatVector::GetDataMutable<int32_t>(vector)[active_row] = static_cast<int32_t>(value);
		break;
	case TPCDSAppendType::DECIMAL64:
		duckdb::FlatVector::GetDataMutable<int64_t>(vector)[active_row] = value;
		break;
	case TPCDSAppendType::DECIMAL128:
		duckdb::FlatVector::GetDataMutable<duckdb::hugeint_t>(vector)[active_row] = value;
		break;
	default:
		throw duckdb::InternalException("Unexpected TPC-DS decimal column type");
	}
}

void append_row_start(append_info info) {
	auto append_info = (tpcds_append_information *)info;
	append_info->BeginRow();
}

void append_row_end(append_info info) {
	auto append_info = (tpcds_append_information *)info;
	append_info->EndRow();
}

void append_varchar(append_info info, const char *value, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn) || !value || *value == '\0') {
		append_info->AppendNull();
	} else {
		append_info->AppendVarchar(value);
	}
}

void append_key(append_info info, int64_t value, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn) || value < 0) {
		append_info->AppendNull();
	} else {
		append_info->AppendKey(value);
	}
}

void append_integer_decimal(append_info info, int32_t val, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn)) {
		append_info->AppendNull();
	} else {
		append_info->AppendIntegerDecimal(val);
	}
}

void append_integer(append_info info, int32_t value, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn)) {
		append_info->AppendNull();
	} else {
		append_info->AppendInteger(value);
	}
}

void append_boolean(append_info info, int32_t value, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn)) {
		append_info->AppendNull();
	} else {
		append_info->AppendBoolean(value);
	}
}

// value is a Julian date
void append_date(append_info info, int64_t value, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn) || value < 0) {
		append_info->AppendNull();
	} else {
		static constexpr int64_t UNIX_EPOCH_JULIAN = 2440588;
		append_info->AppendDate(duckdb::date_t(static_cast<int32_t>(value - UNIX_EPOCH_JULIAN)));
	}
}

void append_decimal(append_info info, decimal_t *val, int nColumn) {
	auto append_info = (tpcds_append_information *)info;
	if (append_info->IsNull(nColumn)) {
		append_info->AppendNull();
		return;
	}
	D_ASSERT(append_info->ActiveDecimalScale() == val->precision);
	append_info->AppendDecimal(val->number);
}
