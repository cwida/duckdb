#include "dbgen/dbgen.hpp"
#include "dbgen/dbgen_gunk.hpp"
#include "tpch_constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/optimistic_data_writer.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#define DECLARER /* EXTERN references get defined here */

#include "dbgen/dss.h"
#include "dbgen/rng64.h"
#include "dbgen/dsstypes.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

using namespace duckdb;

namespace tpch {

enum class TPCHAppendMode : uint8_t { APPENDER, OPTIMISTIC };

struct tpch_append_information {
	duckdb::unique_ptr<InternalAppender> appender;
	unique_ptr<OptimisticDataWriter> optimistic_writer;
	optional_ptr<OptimisticWriteCollection> optimistic_collection;
	optional_ptr<DuckTableEntry> table_entry;
	PhysicalIndex optimistic_collection_index = PhysicalIndex(DConstants::INVALID_INDEX);
	TableAppendState append_state;
	DataChunk chunk;
	idx_t row = 0;
	idx_t active_row = DConstants::INVALID_INDEX;
	idx_t active_col = 0;
	bool finalized = false;
	//! raw column pointers of the current chunk, so the per-value append path does no bounds or type checks
	static constexpr idx_t MAX_COLUMNS = 16;
	Vector *columns[MAX_COLUMNS];
	data_ptr_t column_data[MAX_COLUMNS];

	void RefreshColumnPointers() {
		D_ASSERT(chunk.ColumnCount() <= MAX_COLUMNS);
		for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
			auto &vector = chunk.data[col];
			D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR);
			columns[col] = &vector;
			column_data[col] = FlatVector::GetDataMutable(vector);
		}
	}

	~tpch_append_information() {
		if (optimistic_writer) {
			optimistic_writer->Rollback();
		}
	}

	void Initialize(ClientContext &context, TableCatalogEntry &table, idx_t flush_count) {
		appender = make_uniq<InternalAppender>(context, table, flush_count);
		chunk.Initialize(context, table.GetTypes());
		RefreshColumnPointers();
	}

	void InitializeOptimistic(ClientContext &context, DuckTableEntry &table,
	                          OptimisticWritePartialManagers partial_manager_type) {
		table_entry = table;
		optimistic_writer = make_uniq<OptimisticDataWriter>(context, table.GetStorage());
		auto collection =
		    optimistic_writer->CreateCollection(table.GetStorage(), table.GetTypes(), partial_manager_type);
		auto &row_collection = *collection->collection;
		row_collection.InitializeEmpty();
		// the transaction id keeps the rows invisible to other transactions until the commit
		row_collection.InitializeAppend(DuckTransaction::Get(context, table.catalog), append_state);
		optimistic_collection_index = table.GetStorage().CreateOptimisticCollection(context, std::move(collection));
		optimistic_collection = table.GetStorage().GetOptimisticCollection(context, optimistic_collection_index);
		chunk.Initialize(context, table.GetTypes());
		RefreshColumnPointers();
	}

	void ResetOptimisticCollection(ClientContext &context) {
		D_ASSERT(table_entry);
		D_ASSERT(optimistic_collection_index.IsValid());
		table_entry->GetStorage().ResetOptimisticCollection(context, optimistic_collection_index);
		optimistic_collection_index = PhysicalIndex(DConstants::INVALID_INDEX);
		optimistic_collection = nullptr;
	}

	void FlushChunk() {
		if (row == 0) {
			return;
		}
		D_ASSERT(active_row == DConstants::INVALID_INDEX);
		chunk.SetChildCardinality(row);
		if (appender) {
			appender->AppendDataChunk(chunk);
		} else {
			D_ASSERT(optimistic_collection);
			auto &row_collection = *optimistic_collection->collection;
			auto flushed_row_group_idx = row_collection.Append(chunk, append_state);
			if (flushed_row_group_idx.IsValid()) {
				optimistic_writer->WriteNewRowGroup(*optimistic_collection, flushed_row_group_idx.GetIndex());
			}
		}
		chunk.Reset();
		RefreshColumnPointers();
		row = 0;
		active_col = 0;
	}

	void FlushOptimistic(ClientContext &context) {
		D_ASSERT(table_entry);
		D_ASSERT(optimistic_writer);
		D_ASSERT(optimistic_collection);
		FinalizeOptimisticAppend();

		auto &row_collection = *optimistic_collection->collection;
		auto append_count = row_collection.GetTotalRows();
		if (append_count == 0) {
			ResetOptimisticCollection(context);
			optimistic_writer.reset();
			finalized = false;
			return;
		}

		auto &table = *table_entry;
		auto &storage = table.GetStorage();
		if (append_count < storage.GetRowGroupSize()) {
			auto binder = Binder::CreateBinder(context);
			auto bound_constraints = binder->BindConstraints(table);
			LocalAppendState local_append_state;
			storage.InitializeLocalAppend(local_append_state, table, context, bound_constraints);
			auto &transaction = DuckTransaction::Get(context, table.catalog);
			for (auto &insert_chunk : row_collection.Chunks(transaction)) {
				storage.LocalAppend(local_append_state, table, context, insert_chunk, false);
			}
			storage.FinalizeLocalAppend(local_append_state);
			ResetOptimisticCollection(context);
		} else {
			optimistic_writer->WriteUnflushedRowGroups(*optimistic_collection);
			storage.LocalMerge(context, table, *optimistic_collection);
			auto &storage_writer = storage.GetOptimisticWriter(context);
			storage_writer.Merge(*optimistic_writer);
			ResetOptimisticCollection(context);
		}
		optimistic_writer.reset();
		finalized = false;
	}

	void Flush(ClientContext &context) {
		FlushChunk();
		if (appender) {
			appender->Flush();
			appender.reset();
		} else if (optimistic_collection) {
			FlushOptimistic(context);
		}
	}

	void FinalizeOptimisticAppend() {
		D_ASSERT(optimistic_collection);
		if (finalized) {
			return;
		}
		FlushChunk();
		auto &row_collection = *optimistic_collection->collection;
		row_collection.FinalizeAppend(append_state.transaction, append_state);
		finalized = true;
	}

	void PrepareOptimisticWriteToDisk() {
		D_ASSERT(table_entry);
		D_ASSERT(optimistic_writer);
		D_ASSERT(optimistic_collection);
		FinalizeOptimisticAppend();
		auto &storage = table_entry->GetStorage();
		auto &row_collection = *optimistic_collection->collection;
		if (row_collection.GetTotalRows() >= storage.GetRowGroupSize()) {
			optimistic_writer->WriteUnflushedRowGroups(*optimistic_collection);
			optimistic_writer->FinalFlush();
		}
	}
};

static void append_begin_row(tpch_append_information &info) {
	D_ASSERT(info.appender || info.optimistic_collection);
	D_ASSERT(info.active_row == DConstants::INVALID_INDEX);
	if (info.row >= STANDARD_VECTOR_SIZE) {
		info.FlushChunk();
	}
	info.active_row = info.row;
	info.active_col = 0;
}

static void append_end_row(tpch_append_information &info) {
	D_ASSERT(info.active_row != DConstants::INVALID_INDEX);
	D_ASSERT(info.active_col == info.chunk.ColumnCount());
	info.row++;
	info.active_row = DConstants::INVALID_INDEX;
}

static Vector &append_next_column(tpch_append_information &info) {
	D_ASSERT(info.active_row != DConstants::INVALID_INDEX);
	D_ASSERT(info.active_col < info.chunk.ColumnCount());
	return *info.columns[info.active_col++];
}

template <class T>
static T *append_next_column_data(tpch_append_information &info) {
	D_ASSERT(info.active_row != DConstants::INVALID_INDEX);
	D_ASSERT(info.active_col < info.chunk.ColumnCount());
	D_ASSERT(info.chunk.data[info.active_col].GetType().InternalType() == GetTypeId<T>());
	return reinterpret_cast<T *>(info.column_data[info.active_col++]);
}

void append_int32(tpch_append_information &info, int32_t value) {
	append_next_column_data<int32_t>(info)[info.active_row] = value;
}

void append_int64(tpch_append_information &info, int64_t value) {
	append_next_column_data<int64_t>(info)[info.active_row] = value;
}

void append_string_reference(tpch_append_information &info, const char *value, idx_t length) {
	// Only use for stable DBGEN strings; non-inlined string_t values keep a pointer until the chunk is appended.
	append_next_column_data<string_t>(info)[info.active_row] = string_t(value, UnsafeNumericCast<uint32_t>(length));
}

void append_decimal(tpch_append_information &info, int64_t value) {
	append_next_column_data<int64_t>(info)[info.active_row] = value;
}

void append_char(tpch_append_information &info, char value) {
	auto &vector = append_next_column(info);
	FlatVector::GetDataMutable<string_t>(vector)[info.active_row] = StringVector::AddString(vector, &value, 1);
}

static date_t raw_tpch_date(DSS_HUGE value) {
	static constexpr int32_t UNIX_EPOCH_OFFSET = 8035;
	// DBGEN dates are raw day offsets from STARTDATE; DuckDB dates are days since the Unix epoch.
	return date_t(NumericCast<int32_t>(value - STARTDATE + UNIX_EPOCH_OFFSET));
}

void append_date(tpch_append_information &info, DSS_HUGE value) {
	append_next_column_data<date_t>(info)[info.active_row] = raw_tpch_date(value);
}

static string_t &append_empty_string(tpch_append_information &info, idx_t length) {
	auto &vector = *info.columns[info.active_col];
	auto data = append_next_column_data<string_t>(info);
	data[info.active_row] = StringVector::EmptyString(vector, length);
	return data[info.active_row];
}

static int PaddedNumberWidth(DSS_HUGE value, int min_width) {
	int width = 1;
	for (auto remaining = value; remaining >= 10; remaining /= 10) {
		width++;
	}
	return MaxValue(width, min_width);
}

static void WritePaddedNumber(char *target, DSS_HUGE value, int width) {
	for (auto i = width; i > 0; i--) {
		target[i - 1] = char('0' + (value % 10));
		value /= 10;
	}
}

static void AppendTaggedNumber(tpch_append_information &info, const char *tag, idx_t tag_length, DSS_HUGE value,
                               int min_width) {
	auto number_width = PaddedNumberWidth(value, min_width);
	auto &result = append_empty_string(info, tag_length + number_width);
	auto target = result.GetDataWriteable();
	memcpy(target, tag, tag_length);
	WritePaddedNumber(target + tag_length, value, number_width);
	result.Finalize();
}

static void AppendPhone(tpch_append_information &info, DSS_HUGE nation_code, seed_t *seed) {
	DSS_HUGE acode, exchg, number;
	RANDOM(acode, 100, 999, seed);
	RANDOM(exchg, 100, 999, seed);
	RANDOM(number, 1000, 9999, seed);

	auto &result = append_empty_string(info, PHONE_LEN);
	auto target = result.GetDataWriteable();
	WritePaddedNumber(target, 10 + (nation_code % NATIONS_MAX), 2);
	target[2] = '-';
	WritePaddedNumber(target + 3, acode, 3);
	target[6] = '-';
	WritePaddedNumber(target + 7, exchg, 3);
	target[10] = '-';
	WritePaddedNumber(target + 11, number, 4);
	result.Finalize();
}

static void AppendRandomAlphaNumeric(tpch_append_information &info, int average_length, seed_t *seed) {
	static const char ALPHA_NUM[] = "0123456789abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ,";
	DSS_HUGE length, char_int = 0;
	RANDOM(length, int(average_length * V_STR_LOW), int(average_length * V_STR_HGH), seed);

	auto &result = append_empty_string(info, NumericCast<idx_t>(length));
	auto target = result.GetDataWriteable();
	for (DSS_HUGE i = 0; i < length; i++) {
		if (i % 5 == 0) {
			RANDOM(char_int, 0, MAX_LONG, seed);
		}
		target[i] = ALPHA_NUM[char_int & 077];
		char_int >>= 6;
	}
	result.Finalize();
}

static void AppendText(tpch_append_information &info, int average_length, seed_t *seed) {
	const char *source;
	auto length = dbg_text_source(int(average_length * V_STR_LOW), int(average_length * V_STR_HGH), seed, &source);
	// Text pool memory is stable until cleanup_dists(), after all pending chunks have been appended.
	append_string_reference(info, source, NumericCast<idx_t>(length));
}

struct TPCHStringRef {
	const char *data;
	idx_t length;
};

static TPCHStringRef PickDistribution(distribution *set, seed_t *seed) {
	long index = 0;
	DSS_HUGE choice;

	RANDOM(choice, 1, set->list[set->count - 1].weight, seed);
	while (set->list[index].weight < choice) {
		index++;
	}
	auto data = set->list[index].text;
	return TPCHStringRef {data, NumericCast<idx_t>(set->list[index].length)};
}

static void AppendDistribution(tpch_append_information &info, distribution *set, seed_t *seed) {
	auto entry = PickDistribution(set, seed);
	append_string_reference(info, entry.data, entry.length);
}

static DSS_HUGE RetailPrice(DSS_HUGE part_key) {
	DSS_HUGE price = 90000;
	price += (part_key / 10) % 20001;
	price += (part_key % 1000) * 100;
	return price;
}

static DSS_HUGE PartSuppBridge(DBGenContext *ctx, DSS_HUGE part_key, DSS_HUGE supp_num) {
	DSS_HUGE total_supplier_count = ctx->tdefs[SUPP].base * ctx->scale_factor;
	return (part_key + supp_num * (total_supplier_count / SUPP_PER_PART + (long)((part_key - 1) / total_supplier_count))) %
	           total_supplier_count +
	       1;
}

static void AppendPartName(tpch_append_information &info, DBGenContext *ctx) {
	permute_dist(&colors, P_NAME_SCL, &ctx->Seed[P_NAME_SD], ctx);
	idx_t length = 0;
	for (idx_t i = 0; i < P_NAME_SCL; i++) {
		length += NumericCast<idx_t>(colors.list[ctx->permute[i]].length);
	}
	length += P_NAME_SCL - 1;

	auto &result = append_empty_string(info, length);
	auto target = result.GetDataWriteable();
	for (idx_t i = 0; i < P_NAME_SCL; i++) {
		auto &member = colors.list[ctx->permute[i]];
		auto member_length = NumericCast<idx_t>(member.length);
		auto member_text = member.text;
		memcpy(target, member_text, member_length);
		target += member_length;
		if (i + 1 < P_NAME_SCL) {
			*target++ = ' ';
		}
	}
	result.Finalize();
}

static void AppendSupplierComment(tpch_append_information &info, DBGenContext *ctx) {
	const char *source;
	auto length = dbg_text_source(int(S_CMNT_LEN * V_STR_LOW), int(S_CMNT_LEN * V_STR_HGH), &ctx->Seed[S_CMNT_SD],
	                              &source);
	// Supplier comments can be patched with BBB text, so they need writable vector-owned storage.
	auto &result = append_empty_string(info, NumericCast<idx_t>(length));
	auto target = result.GetDataWriteable();
	memcpy(target, source, NumericCast<idx_t>(length));

	DSS_HUGE bad_press, noise, offset, type;
	RANDOM(bad_press, 1, 10000, &ctx->Seed[BBB_CMNT_SD]);
	RANDOM(type, 0, 100, &ctx->Seed[BBB_TYPE_SD]);
	RANDOM(noise, 0, (length - BBB_CMNT_LEN), &ctx->Seed[BBB_JNK_SD]);
	RANDOM(offset, 0, (length - (BBB_CMNT_LEN + noise)), &ctx->Seed[BBB_OFFSET_SD]);
	if (bad_press <= S_CMNT_BBB) {
		type = (type < BBB_DEADBEATS) ? 0 : 1;
		memcpy(target + offset, BBB_BASE, BBB_BASE_LEN);
		if (type == 0) {
			memcpy(target + BBB_BASE_LEN + offset + noise, BBB_COMPLAIN, BBB_TYPE_LEN);
		} else {
			memcpy(target + BBB_BASE_LEN + offset + noise, BBB_COMMEND, BBB_TYPE_LEN);
		}
	}
	result.Finalize();
}

static void AppendManufacturer(tpch_append_information &info, DSS_HUGE manufacturer) {
	AppendTaggedNumber(info, P_MFG_TAG, sizeof(P_MFG_TAG) - 1, manufacturer, 1);
}

static void AppendBrand(tpch_append_information &info, DSS_HUGE manufacturer, DSS_HUGE brand) {
	AppendTaggedNumber(info, P_BRND_TAG, sizeof(P_BRND_TAG) - 1, manufacturer * 10 + brand, 2);
}

static void GenerateOrderLine(DSS_HUGE index, tpch_append_information *info, DBGenContext *ctx) {
	auto &order_info = info[ORDER];
	auto &line_info = info[LINE];
	static const DSS_HUGE CURRENT_DATE_RAW = STARTDATE + unjulian(CURRENTDATE);

	DSS_HUGE order_key;
	mk_sparse(index, &order_key, 0);

	DSS_HUGE cust_key;
	if (ctx->scale_factor >= 30000) {
		dss_random64(&cust_key, O_CKEY_MIN, O_CKEY_MAX, &ctx->Seed[O_CKEY_SD]);
	} else {
		RANDOM(cust_key, O_CKEY_MIN, O_CKEY_MAX, &ctx->Seed[O_CKEY_SD]);
	}
	int delta = 1;
	while (cust_key % CUST_MORTALITY == 0) {
		cust_key += delta;
		cust_key = MIN(cust_key, O_CKEY_MAX);
		delta *= -1;
	}

	DSS_HUGE order_date;
	RANDOM(order_date, O_ODATE_MIN, O_ODATE_MAX, &ctx->Seed[O_ODATE_SD]);
	auto order_priority = PickDistribution(&o_priority_set, &ctx->Seed[O_PRIO_SD]);
	DSS_HUGE clerk_number;
	RANDOM(clerk_number, 1, MAX((ctx->scale_factor * O_CLRK_SCL), O_CLRK_SCL), &ctx->Seed[O_CLRK_SD]);
	const char *order_comment;
	auto order_comment_length =
	    dbg_text_source(int(O_CMNT_LEN * V_STR_LOW), int(O_CMNT_LEN * V_STR_HGH), &ctx->Seed[O_CMNT_SD],
	                    &order_comment);

	DSS_HUGE line_count;
	RANDOM(line_count, O_LCNT_MIN, O_LCNT_MAX, &ctx->Seed[O_LCNT_SD]);

	DSS_HUGE total_price = 0;
	DSS_HUGE closed_line_count = 0;
	for (DSS_HUGE line_number = 0; line_number < line_count; line_number++) {
		DSS_HUGE quantity, discount, tax, part_key, supp_num, ship_date, commit_date, receipt_date;
		RANDOM(quantity, L_QTY_MIN, L_QTY_MAX, &ctx->Seed[L_QTY_SD]);
		RANDOM(discount, L_DCNT_MIN, L_DCNT_MAX, &ctx->Seed[L_DCNT_SD]);
		RANDOM(tax, L_TAX_MIN, L_TAX_MAX, &ctx->Seed[L_TAX_SD]);
		auto ship_instruct = PickDistribution(&l_instruct_set, &ctx->Seed[L_SHIP_SD]);
		auto ship_mode = PickDistribution(&l_smode_set, &ctx->Seed[L_SMODE_SD]);
		const char *line_comment;
		auto line_comment_length =
		    dbg_text_source(int(L_CMNT_LEN * V_STR_LOW), int(L_CMNT_LEN * V_STR_HGH), &ctx->Seed[L_CMNT_SD],
		                    &line_comment);
		if (ctx->scale_factor >= 30000) {
			dss_random64(&part_key, L_PKEY_MIN, L_PKEY_MAX, &ctx->Seed[L_PKEY_SD]);
		} else {
			RANDOM(part_key, L_PKEY_MIN, L_PKEY_MAX, &ctx->Seed[L_PKEY_SD]);
		}
		auto retail_price = RetailPrice(part_key);
		RANDOM(supp_num, 0, 3, &ctx->Seed[L_SKEY_SD]);
		auto supp_key = PartSuppBridge(ctx, part_key, supp_num);
		quantity *= 100;
		auto extended_price = retail_price * quantity / 100;
		total_price += ((extended_price * (100 - discount)) / PENNIES) * (100 + tax) / PENNIES;

		RANDOM(ship_date, L_SDTE_MIN, L_SDTE_MAX, &ctx->Seed[L_SDTE_SD]);
		ship_date += order_date;
		RANDOM(commit_date, L_CDTE_MIN, L_CDTE_MAX, &ctx->Seed[L_CDTE_SD]);
		commit_date += order_date;
		RANDOM(receipt_date, L_RDTE_MIN, L_RDTE_MAX, &ctx->Seed[L_RDTE_SD]);
		receipt_date += ship_date;

		char return_flag = 'N';
		if (receipt_date <= CURRENT_DATE_RAW) {
			return_flag = PickDistribution(&l_rflag_set, &ctx->Seed[L_RFLG_SD]).data[0];
		}
		char line_status;
		if (ship_date <= CURRENT_DATE_RAW) {
			closed_line_count++;
			line_status = 'F';
		} else {
			line_status = 'O';
		}

		append_begin_row(line_info);
		append_int64(line_info, order_key);
		append_int64(line_info, part_key);
		append_int64(line_info, supp_key);
		append_int64(line_info, line_number + 1);
		append_decimal(line_info, quantity);
		append_decimal(line_info, extended_price);
		append_decimal(line_info, discount);
		append_decimal(line_info, tax);
		append_char(line_info, return_flag);
		append_char(line_info, line_status);
		append_date(line_info, ship_date);
		append_date(line_info, commit_date);
		append_date(line_info, receipt_date);
		append_string_reference(line_info, ship_instruct.data, ship_instruct.length);
		append_string_reference(line_info, ship_mode.data, ship_mode.length);
		append_string_reference(line_info, line_comment, NumericCast<idx_t>(line_comment_length));
		append_end_row(line_info);
	}

	char order_status = 'O';
	if (closed_line_count > 0) {
		order_status = 'P';
	}
	if (closed_line_count == line_count) {
		order_status = 'F';
	}

	append_begin_row(order_info);
	append_int64(order_info, order_key);
	append_int64(order_info, cust_key);
	append_char(order_info, order_status);
	append_decimal(order_info, total_price);
	append_date(order_info, order_date);
	append_string_reference(order_info, order_priority.data, order_priority.length);
	AppendTaggedNumber(order_info, O_CLRK_TAG, sizeof(O_CLRK_TAG) - 1, clerk_number, 9);
	append_int32(order_info, 0);
	append_string_reference(order_info, order_comment, NumericCast<idx_t>(order_comment_length));
	append_end_row(order_info);
}

static void GenerateSupplier(DSS_HUGE index, tpch_append_information *info, DBGenContext *ctx) {
	auto &append_info = info[SUPP];
	append_begin_row(append_info);
	append_int64(append_info, index);
	AppendTaggedNumber(append_info, S_NAME_TAG, sizeof(S_NAME_TAG) - 1, index, 9);
	AppendRandomAlphaNumeric(append_info, S_ADDR_LEN, &ctx->Seed[S_ADDR_SD]);
	DSS_HUGE nation_code;
	RANDOM(nation_code, 0, nations.count - 1, &ctx->Seed[S_NTRG_SD]);
	append_int32(append_info, NumericCast<int32_t>(nation_code));
	AppendPhone(append_info, nation_code, &ctx->Seed[S_PHNE_SD]);
	DSS_HUGE acctbal;
	RANDOM(acctbal, S_ABAL_MIN, S_ABAL_MAX, &ctx->Seed[S_ABAL_SD]);
	append_decimal(append_info, acctbal);
	AppendSupplierComment(append_info, ctx);
	append_end_row(append_info);
}

static void GenerateCustomer(DSS_HUGE index, tpch_append_information *info, DBGenContext *ctx) {
	auto &append_info = info[CUST];
	append_begin_row(append_info);
	append_int64(append_info, index);
	AppendTaggedNumber(append_info, C_NAME_TAG, sizeof(C_NAME_TAG) - 1, index, 9);
	AppendRandomAlphaNumeric(append_info, C_ADDR_LEN, &ctx->Seed[C_ADDR_SD]);
	DSS_HUGE nation_code;
	RANDOM(nation_code, 0, nations.count - 1, &ctx->Seed[C_NTRG_SD]);
	append_int32(append_info, NumericCast<int32_t>(nation_code));
	AppendPhone(append_info, nation_code, &ctx->Seed[C_PHNE_SD]);
	DSS_HUGE acctbal;
	RANDOM(acctbal, C_ABAL_MIN, C_ABAL_MAX, &ctx->Seed[C_ABAL_SD]);
	append_decimal(append_info, acctbal);
	AppendDistribution(append_info, &c_mseg_set, &ctx->Seed[C_MSEG_SD]);
	AppendText(append_info, C_CMNT_LEN, &ctx->Seed[C_CMNT_SD]);
	append_end_row(append_info);
}

static void GeneratePartAndPartsupp(DSS_HUGE index, tpch_append_information *info, DBGenContext *ctx) {
	auto &part_info = info[PART];
	append_begin_row(part_info);
	append_int64(part_info, index);
	AppendPartName(part_info, ctx);
	DSS_HUGE manufacturer;
	RANDOM(manufacturer, P_MFG_MIN, P_MFG_MAX, &ctx->Seed[P_MFG_SD]);
	AppendManufacturer(part_info, manufacturer);
	DSS_HUGE brand;
	RANDOM(brand, P_BRND_MIN, P_BRND_MAX, &ctx->Seed[P_BRND_SD]);
	AppendBrand(part_info, manufacturer, brand);
	AppendDistribution(part_info, &p_types_set, &ctx->Seed[P_TYPE_SD]);
	DSS_HUGE size;
	RANDOM(size, P_SIZE_MIN, P_SIZE_MAX, &ctx->Seed[P_SIZE_SD]);
	append_int32(part_info, NumericCast<int32_t>(size));
	AppendDistribution(part_info, &p_cntr_set, &ctx->Seed[P_CNTR_SD]);
	append_decimal(part_info, RetailPrice(index));
	AppendText(part_info, P_CMNT_LEN, &ctx->Seed[P_CMNT_SD]);
	append_end_row(part_info);

	auto &partsupp_info = info[PSUPP];
	for (DSS_HUGE supp_num = 0; supp_num < SUPP_PER_PART; supp_num++) {
		append_begin_row(partsupp_info);
		append_int64(partsupp_info, index);
		append_int64(partsupp_info, PartSuppBridge(ctx, index, supp_num));
		DSS_HUGE quantity;
		RANDOM(quantity, PS_QTY_MIN, PS_QTY_MAX, &ctx->Seed[PS_QTY_SD]);
		append_int64(partsupp_info, quantity);
		DSS_HUGE supply_cost;
		RANDOM(supply_cost, PS_SCST_MIN, PS_SCST_MAX, &ctx->Seed[PS_SCST_SD]);
		append_decimal(partsupp_info, supply_cost);
		AppendText(partsupp_info, PS_CMNT_LEN, &ctx->Seed[PS_CMNT_SD]);
		append_end_row(partsupp_info);
	}
}

static void GenerateNation(DSS_HUGE index, tpch_append_information *info, DBGenContext *ctx) {
	auto &append_info = info[NATION];
	append_begin_row(append_info);
	append_int32(append_info, NumericCast<int32_t>(index - 1));
	append_string_reference(append_info, nations.list[index - 1].text, NumericCast<idx_t>(nations.list[index - 1].length));
	append_int32(append_info, NumericCast<int32_t>(nations.list[index - 1].weight));
	AppendText(append_info, N_CMNT_LEN, &ctx->Seed[N_CMNT_SD]);
	append_end_row(append_info);
}

static void GenerateRegion(DSS_HUGE index, tpch_append_information *info, DBGenContext *ctx) {
	auto &append_info = info[REGION];
	append_begin_row(append_info);
	append_int32(append_info, NumericCast<int32_t>(index - 1));
	append_string_reference(append_info, regions.list[index - 1].text, NumericCast<idx_t>(regions.list[index - 1].length));
	AppendText(append_info, R_CMNT_LEN, &ctx->Seed[R_CMNT_SD]);
	append_end_row(append_info);
}

static void gen_tbl(ClientContext &context, int tnum, DSS_HUGE count, tpch_append_information *info,
                    DBGenContext *dbgen_ctx, idx_t offset = 0) {
	for (DSS_HUGE i = offset + 1; count; count--, i++) {
		if (count % 1000 == 0) {
			context.InterruptCheck();
		}
		row_start(tnum, dbgen_ctx);
		switch (tnum) {
		case LINE:
		case ORDER:
		case ORDER_LINE:
			GenerateOrderLine(i, info, dbgen_ctx);
			break;
		case SUPP:
			GenerateSupplier(i, info, dbgen_ctx);
			break;
		case CUST:
			GenerateCustomer(i, info, dbgen_ctx);
			break;
		case PSUPP:
		case PART:
		case PART_PSUPP:
			GeneratePartAndPartsupp(i, info, dbgen_ctx);
			break;
		case NATION:
			GenerateNation(i, info, dbgen_ctx);
			break;
		case REGION:
			GenerateRegion(i, info, dbgen_ctx);
			break;
		}
		row_stop_h(tnum, dbgen_ctx);
	}
}

string get_table_name(int num) {
	switch (num) {
	case PART:
		return "part";
	case PSUPP:
		return "partsupp";
	case SUPP:
		return "supplier";
	case CUST:
		return "customer";
	case ORDER:
		return "orders";
	case LINE:
		return "lineitem";
	case NATION:
		return "nation";
	case REGION:
		return "region";
	default:
		return "";
	}
}

struct RegionInfo {
	static constexpr char *Name = "region";
	static constexpr idx_t ColumnCount = 3;
	static const char *Columns[];
	static const LogicalType Types[];
};
const char *RegionInfo::Columns[] = {"r_regionkey", "r_name", "r_comment"};
const LogicalType RegionInfo::Types[] = {LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::VARCHAR),
                                         LogicalType(LogicalTypeId::VARCHAR)};

struct NationInfo {
	static constexpr char *Name = "nation";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 4;
	static const LogicalType Types[];
};
const char *NationInfo::Columns[] = {"n_nationkey", "n_name", "n_regionkey", "n_comment"};
const LogicalType NationInfo::Types[] = {LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::VARCHAR),
                                         LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::VARCHAR)};

struct SupplierInfo {
	static constexpr char *Name = "supplier";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 7;
	static const LogicalType Types[];
};
const char *SupplierInfo::Columns[] = {"s_suppkey", "s_name",    "s_address", "s_nationkey",
                                       "s_phone",   "s_acctbal", "s_comment"};
const LogicalType SupplierInfo::Types[] = {LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::VARCHAR),
                                           LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::INTEGER),
                                           LogicalType(LogicalTypeId::VARCHAR), LogicalType::DECIMAL(15, 2),
                                           LogicalType(LogicalTypeId::VARCHAR)};

struct CustomerInfo {
	static constexpr char *Name = "customer";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 8;
	static const LogicalType Types[];
};
const char *CustomerInfo::Columns[] = {"c_custkey", "c_name",    "c_address",    "c_nationkey",
                                       "c_phone",   "c_acctbal", "c_mktsegment", "c_comment"};
const LogicalType CustomerInfo::Types[] = {LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::VARCHAR),
                                           LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::INTEGER),
                                           LogicalType(LogicalTypeId::VARCHAR), LogicalType::DECIMAL(15, 2),
                                           LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR)};

struct PartInfo {
	static constexpr char *Name = "part";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 9;
	static const LogicalType Types[];
};
const char *PartInfo::Columns[] = {"p_partkey", "p_name",      "p_mfgr",        "p_brand",  "p_type",
                                   "p_size",    "p_container", "p_retailprice", "p_comment"};
const LogicalType PartInfo::Types[] = {
    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::INTEGER),
    LogicalType(LogicalTypeId::VARCHAR), LogicalType::DECIMAL(15, 2),         LogicalType(LogicalTypeId::VARCHAR)};

struct PartsuppInfo {
	static constexpr char *Name = "partsupp";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 5;
	static const LogicalType Types[];
};
const char *PartsuppInfo::Columns[] = {"ps_partkey", "ps_suppkey", "ps_availqty", "ps_supplycost", "ps_comment"};
const LogicalType PartsuppInfo::Types[] = {LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
                                           LogicalType(LogicalTypeId::BIGINT), LogicalType::DECIMAL(15, 2),
                                           LogicalType(LogicalTypeId::VARCHAR)};

struct OrdersInfo {
	static constexpr char *Name = "orders";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 9;
	static const LogicalType Types[];
};
const char *OrdersInfo::Columns[] = {"o_orderkey",      "o_custkey", "o_orderstatus",  "o_totalprice", "o_orderdate",
                                     "o_orderpriority", "o_clerk",   "o_shippriority", "o_comment"};
const LogicalType OrdersInfo::Types[] = {
    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::VARCHAR),
    LogicalType::DECIMAL(15, 2),         LogicalType(LogicalTypeId::DATE),    LogicalType(LogicalTypeId::VARCHAR),
    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::VARCHAR)};

struct LineitemInfo {
	static constexpr char *Name = "lineitem";
	static const char *Columns[];
	static constexpr idx_t ColumnCount = 16;
	static const LogicalType Types[];
};
const char *LineitemInfo::Columns[] = {"l_orderkey",    "l_partkey",       "l_suppkey",  "l_linenumber",
                                       "l_quantity",    "l_extendedprice", "l_discount", "l_tax",
                                       "l_returnflag",  "l_linestatus",    "l_shipdate", "l_commitdate",
                                       "l_receiptdate", "l_shipinstruct",  "l_shipmode", "l_comment"};
const LogicalType LineitemInfo::Types[] = {
    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::BIGINT),
    LogicalType(LogicalTypeId::BIGINT),  LogicalType::DECIMAL(15, 2),         LogicalType::DECIMAL(15, 2),
    LogicalType::DECIMAL(15, 2),         LogicalType::DECIMAL(15, 2),         LogicalType(LogicalTypeId::VARCHAR),
    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DATE),    LogicalType(LogicalTypeId::DATE),
    LogicalType(LogicalTypeId::DATE),    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
    LogicalType(LogicalTypeId::VARCHAR)};

template <class T>
static void ValidateTPCHTableSchema(const TableCatalogEntry &table, const string &table_name) {
	const auto &columns = table.GetColumns();

	if (columns.LogicalColumnCount() != T::ColumnCount) {
		throw InvalidInputException(
		    "TPC-H table \"%s\" has an incompatible schema: expected %llu columns but found %llu", table_name,
		    (unsigned long long)T::ColumnCount, (unsigned long long)columns.LogicalColumnCount());
	}

	for (idx_t i = 0; i < T::ColumnCount; i++) {
		const auto &column = columns.GetColumn(LogicalIndex(i));

		if (column.Name() != Identifier(T::Columns[i])) {
			throw InvalidInputException(
			    "TPC-H table \"%s\" has an incompatible schema: expected column \"%s\" at position %llu but found %s",
			    table_name, T::Columns[i], (unsigned long long)i, column.Name());
		}

		if (column.Type() != T::Types[i]) {
			throw InvalidInputException(
			    "TPC-H table \"%s\" has an incompatible schema: column \"%s\" has type %s but expected %s",
			    table_name, T::Columns[i], column.Type().ToString(), T::Types[i].ToString());
		}
	}
}

template <class T>
static void CreateTPCHTable(ClientContext &context, const Identifier &catalog_name, const Identifier &schema,
                            string suffix) {
	auto table_name = string(T::Name) + suffix;
	auto qualified_name = QualifiedName(catalog_name, schema, Identifier(table_name));

	auto info = make_uniq<CreateTableInfo>();
	info->SetQualifiedName(qualified_name);
	info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	info->temporary = false;
	for (idx_t i = 0; i < T::ColumnCount; i++) {
		info->columns.AddColumn(ColumnDefinition(T::Columns[i], T::Types[i]));
		info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(i)));
	}
	auto &catalog = Catalog::GetCatalog(context, catalog_name);
	catalog.CreateTable(context, std::move(info));

	// Validate both newly created and pre-existing TPC-H tables before appending.
	auto &table = catalog.GetEntry<TableCatalogEntry>(context, qualified_name);
	ValidateTPCHTableSchema<T>(table, table_name);
}

void DBGenWrapper::CreateTPCHSchema(ClientContext &context, const Identifier &catalog, const Identifier &schema,
                                    string suffix) {
	CreateTPCHTable<RegionInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<NationInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<SupplierInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<CustomerInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<PartInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<PartsuppInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<OrdersInfo>(context, catalog, schema, suffix);
	CreateTPCHTable<LineitemInfo>(context, catalog, schema, suffix);
}

void skip(int table, int children, DSS_HUGE step, DBGenContext &dbgen_ctx) {
	switch (table) {
	case CUST:
		sd_cust(children, step, &dbgen_ctx);
		break;
	case SUPP:
		sd_supp(children, step, &dbgen_ctx);
		break;
	case NATION:
		sd_nation(children, step, &dbgen_ctx);
		break;
	case REGION:
		sd_region(children, step, &dbgen_ctx);
		break;
	case ORDER_LINE:
		sd_line(children, step, &dbgen_ctx);
		sd_order(children, step, &dbgen_ctx);
		break;
	case PART_PSUPP:
		sd_part(children, step, &dbgen_ctx);
		sd_psupp(children, step, &dbgen_ctx);
		break;
	}
}

struct TPCHDBgenParameters {
	TPCHDBgenParameters(ClientContext &context, Catalog &catalog, const Identifier &schema, const string &suffix) {
		tables.resize(REGION + 1);
		for (size_t i = PART; i <= REGION; i++) {
			auto tname = get_table_name(i);
			if (!tname.empty()) {
				string full_tname = string(tname) + string(suffix);
				auto &tbl_catalog = catalog.GetEntry<TableCatalogEntry>(
				    context, QualifiedName(catalog.GetName(), schema, Identifier(full_tname)));
				tables[i] = &tbl_catalog;
			}
		}
	}

	vector<optional_ptr<TableCatalogEntry>> tables;
};

static constexpr int DBGEN_TABLES[] = {SUPP, CUST, ORDER_LINE, PART_PSUPP, NATION, REGION};
static constexpr idx_t DBGEN_ROW_BATCH_SIZE = 100000;
static constexpr idx_t DBGEN_TARGET_CHUNK_ROWS = 2 * DEFAULT_ROW_GROUP_SIZE;

struct DBGenTableRange {
	idx_t offset;
	idx_t count;
};

static idx_t GetDBGenTableRowCount(const DBGenContext &dbgen_ctx, int table_index) {
	if (table_index < NATION) {
		return static_cast<idx_t>(dbgen_ctx.tdefs[table_index].base * dbgen_ctx.scale_factor);
	}
	return static_cast<idx_t>(dbgen_ctx.tdefs[table_index].base);
}

static DBGenTableRange GetDBGenTableRange(idx_t row_count, int children, int current_step) {
	if (children <= 1 || current_step == -1) {
		return DBGenTableRange {0, row_count};
	}
	auto child_count = static_cast<idx_t>(children);
	auto step = static_cast<idx_t>(current_step);
	auto part_size = (row_count + child_count - 1) / child_count;
	auto part_offset = part_size * step;
	if (part_offset >= row_count) {
		return DBGenTableRange {part_offset, 0};
	}
	return DBGenTableRange {part_offset, MinValue<idx_t>(part_size, row_count - part_offset)};
}

static idx_t GetDBGenTotalWork(const DBGenContext &dbgen_ctx, int children, int start_step, int end_step) {
	idx_t total = 0;
	for (int step = start_step; step < end_step; step++) {
		for (auto table_index : DBGEN_TABLES) {
			auto row_count = GetDBGenTableRowCount(dbgen_ctx, table_index);
			total += GetDBGenTableRange(row_count, children, step).count;
		}
	}
	return total;
}

struct DBGenWorkItem {
	int table_index;
	int children;
	int step;
	idx_t count;
};

static bool DBGenPhysicalTableMatches(int table_index, int physical_table_index) {
	switch (table_index) {
	case ORDER_LINE:
		return physical_table_index == ORDER || physical_table_index == LINE;
	case PART_PSUPP:
		return physical_table_index == PART || physical_table_index == PSUPP;
	default:
		return physical_table_index == table_index;
	}
}

static idx_t GetDBGenTableOutputMultiplier(int table_index) {
	switch (table_index) {
	case ORDER_LINE:
		return 5;
	case PART_PSUPP:
		return SUPP_PER_PART + 1;
	default:
		return 1;
	}
}

static idx_t GetDBGenTableOrder(int table_index) {
	for (idx_t i = 0; i < sizeof(DBGEN_TABLES) / sizeof(DBGEN_TABLES[0]); i++) {
		if (DBGEN_TABLES[i] == table_index) {
			return i;
		}
	}
	throw InternalException("Unexpected TPC-H dbgen table index");
}

static idx_t GetDefaultTableChildCount(const DBGenContext &dbgen_ctx, int table_index, idx_t thread_count) {
	auto row_count = GetDBGenTableRowCount(dbgen_ctx, table_index);
	if (row_count <= DBGEN_TARGET_CHUNK_ROWS || thread_count <= 1) {
		return 1;
	}
	auto child_count = (row_count + DBGEN_TARGET_CHUNK_ROWS - 1) / DBGEN_TARGET_CHUNK_ROWS;
	return MinValue<idx_t>(child_count, MAX_CHILDREN);
}

class TPCHDataAppender {
public:
	TPCHDataAppender(ClientContext &context, TPCHDBgenParameters &parameters, DBGenContext base_context,
	                 TPCHAppendMode append_mode, idx_t flush_count,
	                 optional_ptr<atomic<idx_t>> generated_work_counter = nullptr, int target_table = -1)
	    : context(context), parameters(parameters), append_mode(append_mode),
	      generated_work_counter(generated_work_counter) {
		dbgen_ctx = base_context;
		append_info = duckdb::unique_ptr<tpch_append_information[]>(new tpch_append_information[REGION + 1]);
		for (size_t i = PART; i <= REGION; i++) {
			if (target_table != -1 && !DBGenPhysicalTableMatches(target_table, NumericCast<int>(i))) {
				continue;
			}
			if (parameters.tables[i]) {
				auto &tbl_catalog = *parameters.tables[i];
				auto duck_table = tbl_catalog.TryGetDuckTableEntry();
				if (!duck_table) {
					throw InvalidInputException("dbgen is only supported for DuckDB database files");
				}
				switch (append_mode) {
				case TPCHAppendMode::APPENDER:
					append_info[i].Initialize(context, tbl_catalog, flush_count);
					break;
				case TPCHAppendMode::OPTIMISTIC: {
					auto partial_manager_type = i == LINE ? OptimisticWritePartialManagers::GLOBAL
					                                      : OptimisticWritePartialManagers::PER_COLUMN;
					append_info[i].InitializeOptimistic(context, *duck_table, partial_manager_type);
					break;
				}
				default:
					throw InternalException("Unsupported TPC-H append mode");
				}
			}
		}
	}

	void GenerateTableData(int table_index, idx_t row_count, idx_t offset) {
		if (row_count == 0) {
			return;
		}
		gen_tbl(context, table_index, static_cast<DSS_HUGE>(row_count), append_info.get(), &dbgen_ctx, offset);
		generated_work += row_count;
		if (generated_work_counter) {
			generated_work_counter->fetch_add(row_count);
		}
	}

	void SkipTable(int table_index, int children, idx_t offset) {
		if (children > 1) {
			skip(table_index, children, static_cast<DSS_HUGE>(offset), dbgen_ctx);
		}
	}

	void AppendData(int children, int current_step) {
		for (auto table_index : DBGEN_TABLES) {
			AppendTableData(table_index, children, current_step);
		}
	}

	void AppendTableData(int table_index, int children, int current_step) {
		if (!(table & (1 << table_index))) {
			return;
		}
		auto rowcnt = GetDBGenTableRowCount(dbgen_ctx, table_index);
		context.InterruptCheck();
		if (children > 1 && current_step != -1) {
			auto range = GetDBGenTableRange(rowcnt, children, current_step);
			SkipTable(table_index, children, range.offset);
			if (range.count > 0) {
				// generate part of the table
				GenerateTableData(table_index, range.count, range.offset);
			}
		} else {
			// generate full table
			GenerateTableData(table_index, rowcnt, 0);
		}
	}

	void Flush() {
		// flush any incomplete chunks
		for (idx_t i = PART; i <= REGION; i++) {
			if (append_info[i].appender || append_info[i].optimistic_collection) {
				append_info[i].Flush(context);
			}
		}
	}

	void PrepareOptimisticWrites() {
		if (append_mode != TPCHAppendMode::OPTIMISTIC) {
			return;
		}
		for (idx_t i = PART; i <= REGION; i++) {
			if (append_info[i].optimistic_collection) {
				append_info[i].PrepareOptimisticWriteToDisk();
			}
		}
	}

	idx_t GeneratedWork() const {
		return generated_work;
	}

private:
	ClientContext &context;
	TPCHDBgenParameters &parameters;
	TPCHAppendMode append_mode;
	optional_ptr<atomic<idx_t>> generated_work_counter;
	idx_t generated_work = 0;
	unique_ptr<tpch_append_information[]> append_info;
	DBGenContext dbgen_ctx;
};

struct FinishedDBGenAppender {
	FinishedDBGenAppender(DBGenWorkItem work_item, unique_ptr<TPCHDataAppender> appender)
	    : work_item(work_item), appender(std::move(appender)) {
	}

	DBGenWorkItem work_item;
	unique_ptr<TPCHDataAppender> appender;
};

class ParallelTPCHAppendTask : public BaseExecutorTask {
public:
	ParallelTPCHAppendTask(TaskExecutor &executor, TPCHDataAppender &appender, DBGenWorkItem work_item)
	    : BaseExecutorTask(executor), appender(appender), work_item(work_item) {
	}

	void ExecuteTask() override {
		appender.AppendTableData(work_item.table_index, work_item.children, work_item.step);
		appender.PrepareOptimisticWrites();
	}

	string TaskType() const override {
		return "ParallelTPCHAppend";
	}

private:
	TPCHDataAppender &appender;
	DBGenWorkItem work_item;
};

DBGenGenerator::~DBGenGenerator() {
}

class TPCHDBGenGenerator : public DBGenGenerator {
public:
	TPCHDBGenGenerator(ClientContext &context, double flt_scale, const Identifier &catalog_name,
	                   const Identifier &schema, string suffix, int children, int current_step)
	    : context(context), flt_scale(flt_scale), children(children), current_step(current_step) {
		// NaN compares false against every scale factor check, and reaches an out-of-range cast below
		if (Value::IsNan(flt_scale)) {
			throw InvalidInputException("DBGen requires a valid scale factor.");
		}

		if (flt_scale <= 0) {
			Finish();
			return;
		}

		if (flt_scale > MAX_SCALE) {
			throw InvalidInputException("DBGen does not support a scale factor exceeding %d.",
			                            static_cast<int>(MAX_SCALE));
		}

		InitializeBaseContext();

		if (current_step >= children) {
			Finish();
			return;
		}

		auto &catalog = Catalog::GetCatalog(context, catalog_name);
		parameters = make_uniq<TPCHDBgenParameters>(context, catalog, schema, suffix);

		load_dists(TEXT_POOL_SIZE, &base_context);
		distributions_loaded = true;
		/* have to do this after init */
		base_context.tdefs[NATION].base = nations.count;
		base_context.tdefs[REGION].base = regions.count;

#ifndef DUCKDB_NO_THREADS
		auto thread_count = TaskScheduler::GetScheduler(context).NumberOfThreads();
		if (!ExplicitPartialGeneration() && thread_count > 1) {
			mode = DBGenMode::PARALLEL;
			parallel_thread_count = thread_count;
			InitializeParallelWorkItems();
			return;
		}
#endif

		mode = DBGenMode::SEQUENTIAL;
		sequential_children = ExplicitPartialGeneration() ? children : NumericCast<int>(GetDefaultChildCount());
		sequential_start_step = ExplicitPartialGeneration() ? current_step : 0;
		sequential_end_step = ExplicitPartialGeneration() ? current_step + 1 : sequential_children;
		sequential_step = sequential_start_step;
		total_work = GetDBGenTotalWork(base_context, sequential_children, sequential_start_step, sequential_end_step);
	}

	~TPCHDBGenGenerator() override {
		if (distributions_loaded) {
			cleanup_dists();
		}
	}

	bool GenerateNext() override {
		context.InterruptCheck();
		if (finished.load()) {
			return true;
		}
		if (total_work == 0) {
			Finish();
			return true;
		}
		switch (mode) {
		case DBGenMode::PARALLEL:
			return GenerateParallel();
		case DBGenMode::SEQUENTIAL:
			return GenerateSequential();
		default:
			throw InternalException("Unexpected TPC-H dbgen mode");
		}
	}

	double Progress() const override {
		if (finished.load()) {
			return 100.0;
		}
		if (total_work == 0) {
			return 0.0;
		}
		auto current_generated_work = MinValue<idx_t>(generated_work.load(), total_work);
		auto current_flushed_work = MinValue<idx_t>(flushed_work.load(), total_work);
		auto generated_progress = static_cast<double>(current_generated_work) / static_cast<double>(total_work);
		auto flushed_progress = static_cast<double>(current_flushed_work) / static_cast<double>(total_work);
		auto flush_weight = mode == DBGenMode::PARALLEL ? 0.05 : 0.5;
		auto current_progress =
		    100.0 * (generated_progress * (1.0 - flush_weight) + flushed_progress * flush_weight);
		return MinValue<double>(current_progress, 99.9);
	}

private:
	enum class DBGenMode : uint8_t { SEQUENTIAL, PARALLEL };

	void InitializeBaseContext() {
		// all tables
		table = (1 << CUST) | (1 << SUPP) | (1 << NATION) | (1 << REGION) | (1 << PART_PSUPP) | (1 << ORDER_LINE);
		force = 0;
		insert_segments = 0;
		delete_segments = 0;
		insert_orders_segment = 0;
		insert_lineitem_segment = 0;
		delete_segment = 0;
		verbose = 0;
		set_seeds = 0;
		updates = 0;

		d_path = NULL;

		auto tdefs = base_context.tdefs;
		tdefs[PART].base = 200000;
		tdefs[PSUPP].base = 200000;
		tdefs[SUPP].base = 10000;
		tdefs[CUST].base = 150000;
		tdefs[ORDER].base = 150000 * ORDERS_PER_CUST;
		tdefs[LINE].base = 150000 * ORDERS_PER_CUST;
		tdefs[ORDER_LINE].base = 150000 * ORDERS_PER_CUST;
		tdefs[PART_PSUPP].base = 200000;
		tdefs[NATION].base = NATIONS_MAX;
		tdefs[REGION].base = NATIONS_MAX;

		if (flt_scale < MIN_SCALE) {
			auto int_scale = static_cast<int>(1000 * flt_scale);
			base_context.scale_factor = 1;
			for (int i = PART; i < REGION; i++) {
				tdefs[i].base = (DSS_HUGE)(int_scale * tdefs[i].base) / 1000;
				if (tdefs[i].base < 1) {
					tdefs[i].base = 1;
				}
			}
		} else {
			base_context.scale_factor = static_cast<long>(flt_scale);
		}
	}

	bool ExplicitPartialGeneration() const {
		return children > 1 && current_step != -1;
	}

	idx_t GetDefaultChildCount() const {
		if (flt_scale < 1) {
			return 1;
		}
		static constexpr idx_t CHILDREN_PER_SCALE_FACTOR = 20;
		return MinValue<idx_t>(static_cast<idx_t>(CHILDREN_PER_SCALE_FACTOR * flt_scale), MAX_CHILDREN);
	}

	bool UseStatsCompatibleChildCount(int table_index) const {
		// Keep small join dimensions on the historical batch boundaries used by plan-cost-sensitive statistics.
		switch (table_index) {
		case SUPP:
		case CUST:
			return true;
		default:
			return false;
		}
	}

	void InitializeParallelWorkItems() {
		total_work = 0;
		parallel_work_items.clear();
		for (auto table_index : DBGEN_TABLES) {
			if (!(table & (1 << table_index))) {
				continue;
			}
			auto children = UseStatsCompatibleChildCount(table_index)
			                    ? NumericCast<int>(GetDefaultChildCount())
			                    : NumericCast<int>(
			                          GetDefaultTableChildCount(base_context, table_index, parallel_thread_count));
			for (int step = 0; step < children; step++) {
				auto row_count = GetDBGenTableRowCount(base_context, table_index);
				auto range = GetDBGenTableRange(row_count, children, step);
				if (range.count == 0) {
					continue;
				}
				parallel_work_items.push_back(DBGenWorkItem {table_index, children, step, range.count});
				total_work += range.count;
			}
		}
		std::sort(parallel_work_items.begin(), parallel_work_items.end(),
		          [](const DBGenWorkItem &left, const DBGenWorkItem &right) {
			          auto left_output_rows = left.count * GetDBGenTableOutputMultiplier(left.table_index);
			          auto right_output_rows = right.count * GetDBGenTableOutputMultiplier(right.table_index);
			          if (left_output_rows != right_output_rows) {
				          return left_output_rows > right_output_rows;
			          }
			          auto left_table_order = GetDBGenTableOrder(left.table_index);
			          auto right_table_order = GetDBGenTableOrder(right.table_index);
			          if (left_table_order != right_table_order) {
				          return left_table_order < right_table_order;
			          }
			          return left.step < right.step;
		          });
		parallel_next_flush_step.assign(REGION + 1, 0);
	}

	bool FlushNextFinishedAppender() {
		for (idx_t appender_idx = 0; appender_idx < finished_appenders.size(); appender_idx++) {
			auto &finished_appender = *finished_appenders[appender_idx];
			auto table_index = finished_appender.work_item.table_index;
			if (finished_appender.work_item.step != parallel_next_flush_step[table_index]) {
				continue;
			}
			auto flushed_count = finished_appender.appender->GeneratedWork();
			finished_appender.appender->Flush();
			flushed_work.fetch_add(flushed_count);
			parallel_next_flush_step[table_index]++;
			finished_appenders.erase(finished_appenders.begin() + UnsafeNumericCast<int64_t>(appender_idx));
			return true;
		}
		return false;
	}

	void FlushAllFinishedAppenders() {
		while (FlushNextFinishedAppender()) {
		}
		if (!finished_appenders.empty()) {
			throw InternalException("Failed to flush TPC-H dbgen appenders in deterministic order");
		}
	}

	bool GenerateParallel() {
#ifdef DUCKDB_NO_THREADS
		throw InternalException("Parallel TPC-H dbgen selected without threading support");
#else
		if (FlushNextFinishedAppender()) {
			return false;
		}
		if (parallel_work_offset < parallel_work_items.size()) {
			TaskExecutor executor(context);

			vector<unique_ptr<TPCHDataAppender>> new_appenders;
			new_appenders.reserve(parallel_thread_count);
			auto launched_offset = parallel_work_offset;
			for (idx_t thr_idx = 0; thr_idx < parallel_thread_count && launched_offset < parallel_work_items.size();
			     thr_idx++, launched_offset++) {
				auto &work_item = parallel_work_items[launched_offset];
				new_appenders.push_back(make_uniq<TPCHDataAppender>(
				    context, *parameters, base_context, TPCHAppendMode::OPTIMISTIC,
				    NumericLimits<int64_t>::Maximum(), &generated_work, work_item.table_index));
			}
			for (auto &appender : new_appenders) {
				auto task = make_uniq<ParallelTPCHAppendTask>(executor, *appender,
				                                              parallel_work_items[parallel_work_offset]);
				executor.ScheduleTask(std::move(task));
				parallel_work_offset++;
			}
			executor.WorkOnTasks();
			for (idx_t appender_idx = 0; appender_idx < new_appenders.size(); appender_idx++) {
				auto work_item_idx = parallel_work_offset - new_appenders.size() + appender_idx;
				finished_appenders.push_back(make_uniq<FinishedDBGenAppender>(
				    parallel_work_items[work_item_idx], std::move(new_appenders[appender_idx])));
			}
			return false;
		}
		Finish();
		return true;
#endif
	}

	void InitializeSequentialAppender() {
		if (sequential_appender) {
			return;
		}
		sequential_appender =
		    make_uniq<TPCHDataAppender>(context, *parameters, base_context, TPCHAppendMode::APPENDER,
		                                BaseAppender::DEFAULT_FLUSH_COUNT, &generated_work);
		sequential_table = 0;
		sequential_table_started = false;
	}

	bool GenerateSequential() {
		while (sequential_step < sequential_end_step) {
			InitializeSequentialAppender();
			while (sequential_table < sizeof(DBGEN_TABLES) / sizeof(DBGEN_TABLES[0])) {
				auto table_index = DBGEN_TABLES[sequential_table];
				if (!sequential_table_started) {
					auto row_count = GetDBGenTableRowCount(base_context, table_index);
					auto range = GetDBGenTableRange(row_count, sequential_children, sequential_step);
					sequential_table_offset = range.offset;
					sequential_table_count = range.count;
					sequential_table_generated = 0;
					sequential_table_started = true;
					sequential_appender->SkipTable(table_index, sequential_children, sequential_table_offset);
				}
				if (sequential_table_generated >= sequential_table_count) {
					sequential_table++;
					sequential_table_started = false;
					continue;
				}
				auto remaining = sequential_table_count - sequential_table_generated;
				auto generate_count = MinValue<idx_t>(remaining, DBGEN_ROW_BATCH_SIZE);
				sequential_appender->GenerateTableData(table_index, generate_count,
				                                       sequential_table_offset + sequential_table_generated);
				sequential_table_generated += generate_count;
				if (generated_work.load() >= total_work) {
					continue;
				}
				return false;
			}
			flushed_work.fetch_add(sequential_appender->GeneratedWork());
			sequential_appender->Flush();
			sequential_appender.reset();
			sequential_step++;
		}
		Finish();
		return true;
	}

	void Finish() {
		FlushAllFinishedAppenders();
		if (sequential_appender) {
			flushed_work.fetch_add(sequential_appender->GeneratedWork());
			sequential_appender->Flush();
			sequential_appender.reset();
		}
		if (distributions_loaded) {
			cleanup_dists();
			distributions_loaded = false;
		}
		generated_work.store(total_work);
		flushed_work.store(total_work);
		finished.store(true);
	}

private:
	ClientContext &context;
	double flt_scale;
	int children;
	int current_step;
	DBGenContext base_context;
	unique_ptr<TPCHDBgenParameters> parameters;
	bool distributions_loaded = false;
	atomic<bool> finished {false};
	DBGenMode mode = DBGenMode::SEQUENTIAL;
	atomic<idx_t> generated_work {0};
	atomic<idx_t> flushed_work {0};
	idx_t total_work = 0;

	idx_t parallel_thread_count = 1;
	vector<DBGenWorkItem> parallel_work_items;
	idx_t parallel_work_offset = 0;
	vector<unique_ptr<FinishedDBGenAppender>> finished_appenders;
	vector<int> parallel_next_flush_step;

	int sequential_children = 1;
	int sequential_start_step = 0;
	int sequential_end_step = 0;
	int sequential_step = 0;
	idx_t sequential_table = 0;
	bool sequential_table_started = false;
	idx_t sequential_table_offset = 0;
	idx_t sequential_table_count = 0;
	idx_t sequential_table_generated = 0;
	unique_ptr<TPCHDataAppender> sequential_appender;
};

unique_ptr<DBGenGenerator> CreateDBGenGenerator(ClientContext &context, double sf, const Identifier &catalog,
                                                const Identifier &schema, string suffix, int children, int step) {
	return make_uniq<TPCHDBGenGenerator>(context, sf, catalog, schema, std::move(suffix), children, step);
}

void DBGenWrapper::LoadTPCHData(ClientContext &context, double flt_scale, const Identifier &catalog_name,
                                const Identifier &schema, string suffix, int children, int current_step) {
	auto generator = CreateDBGenGenerator(context, flt_scale, catalog_name, schema, std::move(suffix), children,
	                                      current_step);
	while (!generator->GenerateNext()) {
	}
}

string DBGenWrapper::GetQuery(int query) {
	if (query <= 0 || query > TPCH_QUERIES_COUNT) {
		throw SyntaxException("Out of range TPC-H query number %d", query);
	}
	return TPCH_QUERIES[query - 1];
}

//! The stored Q11 carries the FRACTION substitution parameter for SF 1
static constexpr const char *TPCH_Q11_SF1_FRACTION = "0.0001000000";

string DBGenWrapper::GetQuery(int query, double sf) {
	if (Value::IsNan(sf) || sf <= 0 || sf > MAX_SCALE) {
		throw InvalidInputException("TPC-H queries require a scale factor between 0 and %d",
		                            static_cast<int>(MAX_SCALE));
	}
	auto result = GetQuery(query);
	if (query != 11) {
		return result;
	}
	// Q11 selects the parts whose stock value exceeds FRACTION of the total, where qgen sets FRACTION = 0.0001 / SF
	auto fraction = StringUtil::Format("%.10f", 0.0001 / sf);
	auto pos = result.find(TPCH_Q11_SF1_FRACTION);
	D_ASSERT(pos != string::npos);
	return result.replace(pos, strlen(TPCH_Q11_SF1_FRACTION), fraction);
}

string DBGenWrapper::GetAnswer(double sf, int query) {
	if (query <= 0 || query > TPCH_QUERIES_COUNT) {
		throw SyntaxException("Out of range TPC-H query number %d", query);
	}
	const char *answer;
	if (sf == 0.01) {
		answer = TPCH_ANSWERS_SF0_01[query - 1];
	} else if (sf == 0.1) {
		answer = TPCH_ANSWERS_SF0_1[query - 1];
	} else if (sf == 1) {
		answer = TPCH_ANSWERS_SF1[query - 1];
	} else {
		throw NotImplementedException("Don't have TPC-H answers for SF %llf!", sf);
	}
	return answer;
}

} // namespace tpch
