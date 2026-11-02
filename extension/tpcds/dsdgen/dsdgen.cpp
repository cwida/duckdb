#include "dsdgen.hpp"

#include "append_info-c.hpp"
#include "dsdgen_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "tpcds_constants.hpp"
#include "dsdgen_schema.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "dsdgen-c/constants.h"
#include "dsdgen-c/porting.h"
#include "dsdgen-c/parallel.h"
#include "dsdgen-c/tables.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

using namespace duckdb;

namespace tpcds {

static constexpr idx_t DSDGEN_MIN_ROW_BATCH_SIZE = 10000;
static constexpr idx_t DSDGEN_MAX_ROW_BATCH_SIZE = 100000;
static constexpr idx_t DSDGEN_TARGET_CHUNK_ROWS = 2 * DEFAULT_ROW_GROUP_SIZE;
static constexpr idx_t DSDGEN_PROGRESS_UNIT_SCALE = 100;
static constexpr idx_t DSDGEN_MAX_SCALE_FACTOR = 100000;
static constexpr idx_t DSDGEN_PARALLEL_SPLIT_MIN_ROWS = 1000000;
static constexpr idx_t DSDGEN_MIN_PARALLEL_TASK_ROWS = DSDGEN_MIN_ROW_BATCH_SIZE;
// TPC-DS worker initialization creates large per-thread caches, so fanout is bounded.
static constexpr idx_t DSDGEN_WORK_ITEMS_PER_THREAD = 128;
static constexpr idx_t DSDGEN_MAX_WORK_ITEMS_PER_TABLE = 4096;
static constexpr idx_t DSDGEN_PROGRESS_REPORT_ROW_INTERVAL = 10000;

static constexpr int DSDGEN_TABLES[] = {CALL_CENTER,
                                        CATALOG_PAGE,
                                        CATALOG_SALES,
                                        CUSTOMER,
                                        CUSTOMER_ADDRESS,
                                        CUSTOMER_DEMOGRAPHICS,
                                        DATET,
                                        HOUSEHOLD_DEMOGRAPHICS,
                                        INCOME_BAND,
                                        INVENTORY,
                                        ITEM,
                                        PROMOTION,
                                        REASON,
                                        SHIP_MODE,
                                        STORE,
                                        STORE_SALES,
                                        TIME,
                                        WAREHOUSE,
                                        WEB_PAGE,
                                        WEB_SALES,
                                        WEB_SITE};

struct DSDGenTableRange {
	idx_t offset;
	idx_t count;
};

struct DSDGenWorkItem {
	int table_id;
	int children;
	int step;
	idx_t offset;
	idx_t count;
	idx_t progress_units_per_row;
	idx_t progress_units;
};

static int GetDSDGenChildTable(int table_id);

static DSDGenTableRange GetDSDGenTableRange(idx_t row_count, int children, int current_step) {
	if (children <= 1) {
		return DSDGenTableRange {0, row_count};
	}
	auto child_count = static_cast<idx_t>(children);
	auto step = static_cast<idx_t>(current_step);
	auto base_count = row_count / child_count;
	auto extra_rows = row_count % child_count;
	auto offset = (base_count * step) + MinValue<idx_t>(step, extra_rows);
	auto count = base_count + (step < extra_rows ? 1 : 0);
	return DSDGenTableRange {offset, count};
}

static idx_t GetDSDGenTableColumnCount(int table_id) {
	switch (table_id) {
	case CALL_CENTER:
		return CallCenterInfo::ColumnCount;
	case CATALOG_PAGE:
		return CatalogPageInfo::ColumnCount;
	case CATALOG_RETURNS:
		return CatalogReturnsInfo::ColumnCount;
	case CATALOG_SALES:
		return CatalogSalesInfo::ColumnCount;
	case CUSTOMER:
		return CustomerInfo::ColumnCount;
	case CUSTOMER_ADDRESS:
		return CustomerAddressInfo::ColumnCount;
	case CUSTOMER_DEMOGRAPHICS:
		return CustomerDemographicsInfo::ColumnCount;
	case DATET:
		return DateDimInfo::ColumnCount;
	case HOUSEHOLD_DEMOGRAPHICS:
		return HouseholdDemographicsInfo::ColumnCount;
	case INCOME_BAND:
		return IncomeBandInfo::ColumnCount;
	case INVENTORY:
		return InventoryInfo::ColumnCount;
	case ITEM:
		return ItemInfo::ColumnCount;
	case PROMOTION:
		return PromotionInfo::ColumnCount;
	case REASON:
		return ReasonInfo::ColumnCount;
	case SHIP_MODE:
		return ShipModeInfo::ColumnCount;
	case STORE:
		return StoreInfo::ColumnCount;
	case STORE_RETURNS:
		return StoreReturnsInfo::ColumnCount;
	case STORE_SALES:
		return StoreSalesInfo::ColumnCount;
	case TIME:
		return TimeDimInfo::ColumnCount;
	case WAREHOUSE:
		return WarehouseInfo::ColumnCount;
	case WEB_PAGE:
		return WebPageInfo::ColumnCount;
	case WEB_RETURNS:
		return WebReturnsInfo::ColumnCount;
	case WEB_SALES:
		return WebSalesInfo::ColumnCount;
	case WEB_SITE:
		return WebSiteInfo::ColumnCount;
	default:
		throw InternalException("Unexpected TPC-DS table id");
	}
}

static idx_t GetDSDGenTableOutputMultiplier(int table_id) {
	switch (table_id) {
	case CATALOG_SALES:
		return 10;
	case STORE_SALES:
	case WEB_SALES:
		return 12;
	default:
		return 1;
	}
}

static idx_t GetDSDGenTableOrder(int table_id) {
	for (idx_t i = 0; i < sizeof(DSDGEN_TABLES) / sizeof(DSDGEN_TABLES[0]); i++) {
		if (DSDGEN_TABLES[i] == table_id) {
			return i;
		}
	}
	throw InternalException("Unexpected TPC-DS table id");
}

static idx_t GetDSDGenReturnPercent(int table_id) {
	switch (table_id) {
	case CATALOG_SALES:
		return CR_RETURN_PCT;
	case STORE_SALES:
		return SR_RETURN_PCT;
	case WEB_SALES:
		return WR_RETURN_PCT;
	default:
		return 0;
	}
}

static idx_t GetDSDGenTableProgressUnitsPerRow(int table_id) {
	auto output_rows = GetDSDGenTableOutputMultiplier(table_id);
	auto progress_units = output_rows * GetDSDGenTableColumnCount(table_id) * DSDGEN_PROGRESS_UNIT_SCALE;
	auto child_table = GetDSDGenChildTable(table_id);
	if (child_table != -1) {
		progress_units +=
		    output_rows * GetDSDGenReturnPercent(table_id) * GetDSDGenTableColumnCount(child_table) *
		    DSDGEN_PROGRESS_UNIT_SCALE / 100;
	}
	return progress_units;
}

static idx_t GetDSDGenTableProgressUnits(int table_id, idx_t row_count) {
	return row_count * GetDSDGenTableProgressUnitsPerRow(table_id);
}

static idx_t GetDSDGenTableChildCount(int table_id, idx_t row_count, idx_t thread_count) {
	if (row_count == 0 || thread_count <= 1) {
		return 1;
	}
	auto output_row_count = row_count * GetDSDGenTableOutputMultiplier(table_id);
	auto row_count_child_count =
	    output_row_count < DSDGEN_PARALLEL_SPLIT_MIN_ROWS
	        ? 1
	        : (output_row_count + DSDGEN_TARGET_CHUNK_ROWS - 1) / DSDGEN_TARGET_CHUNK_ROWS;
	auto max_child_count =
	    MinValue<idx_t>(thread_count * DSDGEN_WORK_ITEMS_PER_THREAD, DSDGEN_MAX_WORK_ITEMS_PER_TABLE);
	max_child_count = MinValue<idx_t>(max_child_count, MaxValue<idx_t>(1, row_count / DSDGEN_MIN_PARALLEL_TASK_ROWS));
	return MinValue<idx_t>(row_count_child_count, max_child_count);
}

static int GetDSDGenChildTable(int table_id) {
	switch (table_id) {
	case CATALOG_SALES:
		return CATALOG_RETURNS;
	case STORE_SALES:
		return STORE_RETURNS;
	case WEB_SALES:
		return WEB_RETURNS;
	default:
		return -1;
	}
}

static bool DSDGenPhysicalTableMatches(int table_id, int physical_table_id) {
	auto child_table = GetDSDGenChildTable(table_id);
	if (child_table != -1) {
		return physical_table_id == table_id || physical_table_id == child_table;
	}
	return physical_table_id == table_id;
}

static bool IsDSDGenFactTable(int table_id) {
	switch (table_id) {
	case CATALOG_RETURNS:
	case CATALOG_SALES:
	case INVENTORY:
	case STORE_RETURNS:
	case STORE_SALES:
	case WEB_RETURNS:
	case WEB_SALES:
		return true;
	default:
		return false;
	}
}

template <class T>
static void CreateTPCDSTable(ClientContext &context, const Identifier &catalog_name, const Identifier &schema,
                             string suffix, bool keys, bool overwrite) {
	auto info = make_uniq<CreateTableInfo>();
	info->SetQualifiedName(QualifiedName(catalog_name, schema, Identifier(T::Name + suffix)));
	info->on_conflict = overwrite ? OnCreateConflict::REPLACE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	info->temporary = false;
	for (idx_t i = 0; i < T::ColumnCount; i++) {
		info->columns.AddColumn(ColumnDefinition(T::Columns[i], T::Types[i]));
	}
	if (keys) {
		duckdb::vector<duckdb::Identifier> pk_columns;
		for (idx_t i = 0; i < T::PrimaryKeyCount; i++) {
			pk_columns.emplace_back(T::PrimaryKeyColumns[i]);
		}
		info->constraints.push_back(make_uniq<UniqueConstraint>(std::move(pk_columns), true));
	}
	auto &catalog = Catalog::GetCatalog(context, catalog_name);
	catalog.CreateTable(context, std::move(info));
}

void DSDGenWrapper::CreateTPCDSSchema(ClientContext &context, const Identifier &catalog, const Identifier &schema,
                                      string suffix, bool keys, bool overwrite) {
	CreateTPCDSTable<CallCenterInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<CatalogPageInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<CatalogReturnsInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<CatalogSalesInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<CustomerInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<CustomerAddressInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<CustomerDemographicsInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<DateDimInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<HouseholdDemographicsInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<IncomeBandInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<InventoryInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<ItemInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<PromotionInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<ReasonInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<ShipModeInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<StoreInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<StoreReturnsInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<StoreSalesInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<TimeDimInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<WarehouseInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<WebPageInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<WebReturnsInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<WebSalesInfo>(context, catalog, schema, suffix, keys, overwrite);
	CreateTPCDSTable<WebSiteInfo>(context, catalog, schema, suffix, keys, overwrite);
}

DSDGenGenerator::~DSDGenGenerator() {
}

struct TPCDSDSDGenParameters {
	TPCDSDSDGenParameters(ClientContext &context, Catalog &catalog, const Identifier &schema, const string &suffix) {
		tables.resize(DBGEN_VERSION);
		for (auto table_id = CALL_CENTER; table_id < DBGEN_VERSION; table_id++) {
			auto table_def = GetTDefByNumber(table_id);
			auto table_name = string(table_def.name) + suffix;
			auto &table_entry = catalog.GetEntry<TableCatalogEntry>(
			    context, QualifiedName(catalog.GetName(), schema, Identifier(table_name)));
			if (!table_entry.IsDuckTable()) {
				throw InvalidInputException("dsdgen is only supported for DuckDB database files");
			}
			tables[table_id] = table_entry;
		}
	}

	vector<optional_ptr<TableCatalogEntry>> tables;
};

class TPCDSDataAppender {
public:
	TPCDSDataAppender(ClientContext &context, TPCDSDSDGenParameters &parameters, double scale,
	                  TPCDSAppendMode append_mode, idx_t flush_count,
	                  optional_ptr<atomic<idx_t>> generated_work_counter = nullptr, int target_table = -1)
	    : context(context), parameters(parameters), scale(scale), append_mode(append_mode),
	      generated_work_counter(generated_work_counter) {
		append_info.resize(DBGEN_VERSION);
		for (auto table_id = CALL_CENTER; table_id < DBGEN_VERSION; table_id++) {
			if (target_table != -1 && !DSDGenPhysicalTableMatches(target_table, table_id)) {
				continue;
			}
			auto &table_entry = *parameters.tables[table_id];
			auto partial_manager_type = IsDSDGenFactTable(table_id) ? OptimisticWritePartialManagers::GLOBAL
			                                                        : OptimisticWritePartialManagers::PER_COLUMN;
			auto append = make_uniq<tpcds_append_information>(context, &table_entry, append_mode, flush_count,
			                                                  partial_manager_type);
			append->table_def = GetTDefByNumber(table_id);
			append_info[table_id] = std::move(append);
		}
	}

	void AppendTableData(const DSDGenWorkItem &work_item) {
		if (work_item.count == 0) {
			return;
		}
		InitializeDSDgen(scale, work_item.children, work_item.step + 1);
		RefreshAppendTableDefinitions();
		SkipToTableOffset(work_item.table_id, work_item.offset);

		auto builder_func = GetTDefFunctionByNumber(work_item.table_id);
		D_ASSERT(builder_func);
		for (idx_t row_idx = 0; row_idx < work_item.count; row_idx++) {
			auto row_number = static_cast<ds_key_t>(work_item.offset + row_idx + 1);
			if (builder_func((void *)&append_info, row_number)) {
				throw InternalException("Table generation failed");
			}
			row_stop(work_item.table_id);
			if (row_idx % 1000 == 0) {
				context.InterruptCheck();
			}
			if ((row_idx + 1) % DSDGEN_PROGRESS_REPORT_ROW_INTERVAL == 0) {
				ReportGeneratedWork((row_idx + 1) * work_item.progress_units_per_row);
			}
		}
		ReportGeneratedWork(work_item.progress_units);
	}

	void PrepareOptimisticWrites() {
		if (append_mode != TPCDSAppendMode::OPTIMISTIC) {
			return;
		}
		for (auto &append : append_info) {
			if (append) {
				append->PrepareOptimisticWriteToDisk();
			}
		}
	}

	void Flush() {
		for (auto &append : append_info) {
			if (append) {
				append->Close();
			}
		}
	}

	idx_t GeneratedWork() const {
		return generated_work;
	}

private:
	void ReportGeneratedWork(idx_t work) {
		if (work <= generated_work) {
			return;
		}
		auto delta = work - generated_work;
		generated_work = work;
		if (generated_work_counter) {
			generated_work_counter->fetch_add(delta);
		}
	}

	void RefreshAppendTableDefinitions() {
		for (auto table_id = CALL_CENTER; table_id < DBGEN_VERSION; table_id++) {
			if (append_info[table_id]) {
				// Null bitmaps live in thread-local tdefs and must be read by the worker thread.
				append_info[table_id]->table_def = GetTDefByNumber(table_id);
			}
		}
	}

	void SkipToTableOffset(int table_id, idx_t offset) {
		if (offset == 0) {
			return;
		}
		auto skip_count = static_cast<ds_key_t>(offset);
		SkipTableRows(table_id, skip_count);
		auto child_table = GetDSDGenChildTable(table_id);
		if (child_table != -1) {
			SkipTableRows(child_table, skip_count);
		}
	}

private:
	ClientContext &context;
	TPCDSDSDGenParameters &parameters;
	double scale;
	TPCDSAppendMode append_mode;
	optional_ptr<atomic<idx_t>> generated_work_counter;
	idx_t generated_work = 0;
	duckdb::vector<duckdb::unique_ptr<tpcds_append_information>> append_info;
};

struct FinishedDSDGenAppender {
	FinishedDSDGenAppender(DSDGenWorkItem work_item, unique_ptr<TPCDSDataAppender> appender)
	    : work_item(work_item), appender(std::move(appender)) {
	}

	DSDGenWorkItem work_item;
	// Appenders own chunks and append state that must stay stable after worker tasks are scheduled.
	unique_ptr<TPCDSDataAppender> appender;
};

class ParallelTPCDSAppendTask : public BaseExecutorTask {
public:
	ParallelTPCDSAppendTask(TaskExecutor &executor, TPCDSDataAppender &appender, DSDGenWorkItem work_item)
	    : BaseExecutorTask(executor), appender(appender), work_item(work_item) {
	}

	void ExecuteTask() override {
		appender.AppendTableData(work_item);
		appender.PrepareOptimisticWrites();
	}

	string TaskType() const override {
		return "ParallelTPCDSAppend";
	}

private:
	TPCDSDataAppender &appender;
	DSDGenWorkItem work_item;
};

class TPCDSDSDGenGenerator : public DSDGenGenerator {
public:
	TPCDSDSDGenGenerator(ClientContext &context, double scale, const Identifier &catalog_name,
	                     const Identifier &schema, string suffix)
	    : context(context), scale(scale), catalog_name(catalog_name), schema(schema), suffix(std::move(suffix)) {
		if (Value::IsNan(scale)) {
			throw InvalidInputException("DSDGen requires a valid scale factor");
		}

		if (scale <= 0) {
			Finish();
			return;
		}

		if (scale > DSDGEN_MAX_SCALE_FACTOR) {
			throw InvalidInputException("DSDGen does not support a scale factor exceeding %d.",
			                            DSDGEN_MAX_SCALE_FACTOR);
		}

#ifdef DEBUG
		// With a scale of 778+ the following multiplication (signed int target value) during generation is:
		// r->cc_sq_ft *= r->cc_employees overflows;
		// 3,464,105 * 649 = 2,248,204,145 (max. signed int value: 2,147,483,647)
		if (scale > 777) {
			throw InvalidInputException("DSDGen results in a signed integer overflow with a scale exceeding 777.");
		}
#endif

		InitializeDSDgen(scale);
		InitializeWork();

#ifndef DUCKDB_NO_THREADS
		auto thread_count = TaskScheduler::GetScheduler(context).NumberOfThreads();
		parallel_thread_count = thread_count;
		if (thread_count > 1 && HasParallelWork()) {
			mode = DSDGenMode::PARALLEL;
			can_yield = true;
			auto &catalog = Catalog::GetCatalog(context, catalog_name);
			parameters = make_uniq<TPCDSDSDGenParameters>(context, catalog, schema, this->suffix);
			InitializeParallelWorkItems();
			return;
		}
#endif

		mode = DSDGenMode::SEQUENTIAL;
#ifndef DUCKDB_NO_THREADS
		can_yield = parallel_thread_count <= 1;
#else
		can_yield = true;
#endif
		InitializeAppendInfo();
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
		case DSDGenMode::PARALLEL:
			return GenerateParallel();
		case DSDGenMode::SEQUENTIAL:
			return GenerateSequential();
		default:
			throw InternalException("Unexpected TPC-DS dsdgen mode");
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
		auto flush_weight = mode == DSDGenMode::PARALLEL ? 0.05 : 0.0;
		auto current_progress =
		    100.0 * (generated_progress * (1.0 - flush_weight) + flushed_progress * flush_weight);
		return MinValue<double>(current_progress, 99.9);
	}

	bool CanYield() const override {
		return can_yield;
	}

private:
	enum class DSDGenMode : uint8_t { SEQUENTIAL, PARALLEL };

	bool GenerateSequential() {
		EnsureSequentialThreadState();
		idx_t generated_count = 0;
		while (generated_count < row_batch_size) {
			if (!table_started && !StartNextTable()) {
				Finish();
				return true;
			}

			if (current_table_remaining == 0) {
				table_started = false;
				table_id++;
				continue;
			}

			if (builder_func((void *)&append_info, current_row)) {
				throw InternalException("Table generation failed");
			}
			row_stop(table_id);

			current_row++;
			current_table_remaining--;
			generated_work.fetch_add(current_table_progress_units_per_row);
			generated_count++;
			if (generated_count % 1000 == 0) {
				context.InterruptCheck();
			}
		}
		return false;
	}

	void EnsureSequentialThreadState() {
		auto current_thread_id = std::this_thread::get_id();
		if (sequential_thread_initialized && sequential_thread_id == current_thread_id) {
			return;
		}
		InitializeDSDgen(scale);
		RefreshSequentialAppendTableDefinitions();
		if (table_started && current_row > 1) {
			auto skip_count = current_row - 1;
			SkipTableRows(table_id, skip_count);
			auto child_table = GetDSDGenChildTable(table_id);
			if (child_table != -1) {
				SkipTableRows(child_table, skip_count);
			}
		}
		sequential_thread_id = current_thread_id;
		sequential_thread_initialized = true;
	}

	bool GenerateParallel() {
#ifdef DUCKDB_NO_THREADS
		throw InternalException("Parallel TPC-DS dsdgen selected without threading support");
#else
		if (FlushNextFinishedAppender()) {
			return false;
		}
		if (parallel_work_offset < parallel_work_items.size()) {
			TaskExecutor executor(context);

			vector<unique_ptr<TPCDSDataAppender>> new_appenders;
			new_appenders.reserve(parallel_thread_count);
			auto launched_offset = parallel_work_offset;
			for (idx_t thread_idx = 0; thread_idx < parallel_thread_count &&
			                           launched_offset < parallel_work_items.size();
			     thread_idx++, launched_offset++) {
				auto &work_item = parallel_work_items[launched_offset];
				new_appenders.push_back(make_uniq<TPCDSDataAppender>(
				    context, *parameters, scale, TPCDSAppendMode::OPTIMISTIC,
				    NumericLimits<int64_t>::Maximum(), &generated_work, work_item.table_id));
			}
			for (auto &appender : new_appenders) {
				auto task = make_uniq<ParallelTPCDSAppendTask>(executor, *appender,
				                                               parallel_work_items[parallel_work_offset]);
				executor.ScheduleTask(std::move(task));
				parallel_work_offset++;
			}
			executor.WorkOnTasks();
			for (idx_t appender_idx = 0; appender_idx < new_appenders.size(); appender_idx++) {
				auto work_item_idx = parallel_work_offset - new_appenders.size() + appender_idx;
				finished_appenders.push_back(make_uniq<FinishedDSDGenAppender>(
				    parallel_work_items[work_item_idx], std::move(new_appenders[appender_idx])));
			}
			return false;
		}
		Finish();
		return true;
#endif
	}

	void InitializeAppendInfo() {
		append_info.resize(DBGEN_VERSION);
		auto &catalog = Catalog::GetCatalog(context, catalog_name);

		for (int table_idx = CALL_CENTER; table_idx < DBGEN_VERSION; table_idx++) {
			auto table_def = GetTDefByNumber(table_idx);
			auto table_name = table_def.name + suffix;
			D_ASSERT(table_def.name);
			auto &table_entry = catalog.GetEntry<TableCatalogEntry>(
			    context, QualifiedName(catalog.GetName(), schema, Identifier(table_name)));

			if (!table_entry.IsDuckTable()) {
				throw InvalidInputException("dsdgen is only supported for DuckDB database files");
			}

			auto append = make_uniq<tpcds_append_information>(context, &table_entry, TPCDSAppendMode::APPENDER,
			                                                  BaseAppender::DEFAULT_FLUSH_COUNT);
			append->table_def = table_def;
			append_info[table_idx] = std::move(append);
		}
	}

	void RefreshSequentialAppendTableDefinitions() {
		for (auto table_idx = CALL_CENTER; table_idx < DBGEN_VERSION; table_idx++) {
			if (append_info[table_idx]) {
				append_info[table_idx]->table_def = GetTDefByNumber(table_idx);
			}
		}
	}

	void InitializeWork() {
		total_work = 0;
		idx_t total_row_count = 0;
		for (auto table_idx : DSDGEN_TABLES) {
			auto table_def = GetTDefByNumber(table_idx);
			if (table_def.fl_child) {
				continue;
			}
			auto row_count = NumericCast<idx_t>(GetRowCount(table_idx));
			total_row_count += row_count;
			total_work += GetDSDGenTableProgressUnits(table_idx, row_count);
		}
		row_batch_size =
		    MinValue<idx_t>(DSDGEN_MAX_ROW_BATCH_SIZE,
		                    MaxValue<idx_t>(DSDGEN_MIN_ROW_BATCH_SIZE, total_row_count / 1000));
	}

	bool HasParallelWork() const {
		for (auto table_idx : DSDGEN_TABLES) {
			auto row_count = NumericCast<idx_t>(GetRowCount(table_idx));
			if (GetDSDGenTableChildCount(table_idx, row_count, parallel_thread_count) > 1) {
				return true;
			}
		}
		return false;
	}

	void InitializeParallelWorkItems() {
		total_work = 0;
		parallel_work_items.clear();
		for (auto table_idx : DSDGEN_TABLES) {
			auto row_count = NumericCast<idx_t>(GetRowCount(table_idx));
			auto progress_units_per_row = GetDSDGenTableProgressUnitsPerRow(table_idx);
			auto children = NumericCast<int>(GetDSDGenTableChildCount(table_idx, row_count, parallel_thread_count));
			for (auto step = 0; step < children; step++) {
				auto range = GetDSDGenTableRange(row_count, children, step);
				if (range.count == 0) {
					continue;
				}
				auto progress_units = range.count * progress_units_per_row;
				parallel_work_items.push_back(DSDGenWorkItem {table_idx, children, step, range.offset, range.count,
				                                              progress_units_per_row, progress_units});
				total_work += progress_units;
			}
		}
		std::sort(parallel_work_items.begin(), parallel_work_items.end(),
		          [](const DSDGenWorkItem &left, const DSDGenWorkItem &right) {
			          // Run larger work items first; storage publication is ordered separately.
			          if (left.progress_units != right.progress_units) {
				          return left.progress_units > right.progress_units;
			          }
			          auto left_table_order = GetDSDGenTableOrder(left.table_id);
			          auto right_table_order = GetDSDGenTableOrder(right.table_id);
			          if (left_table_order != right_table_order) {
				          return left_table_order < right_table_order;
			          }
			          return left.step < right.step;
		          });
		parallel_next_flush_step.assign(DBGEN_VERSION, 0);
	}

	bool StartNextTable() {
		while (table_id < DBGEN_VERSION) {
			auto &table_def = append_info[table_id]->table_def;
			if (table_def.fl_child) {
				table_id++;
				continue;
			}
			current_row = 1;
			current_table_remaining = GetRowCount(table_id);
			current_table_progress_units_per_row = GetDSDGenTableProgressUnitsPerRow(table_id);
			if (table_def.fl_small) {
				ResetCountCount();
			}
			builder_func = GetTDefFunctionByNumber(table_id);
			D_ASSERT(builder_func);
			table_started = true;
			return true;
		}
		return false;
	}

	bool FlushNextFinishedAppender() {
		for (idx_t appender_idx = 0; appender_idx < finished_appenders.size(); appender_idx++) {
			auto &finished_appender = *finished_appenders[appender_idx];
			auto table_id = finished_appender.work_item.table_id;
			// Merge each table in generated row order, independent of task scheduling order.
			if (finished_appender.work_item.step != parallel_next_flush_step[table_id]) {
				continue;
			}
			auto flushed_count = finished_appender.appender->GeneratedWork();
			finished_appender.appender->Flush();
			flushed_work.fetch_add(flushed_count);
			parallel_next_flush_step[table_id]++;
			finished_appenders.erase(finished_appenders.begin() + UnsafeNumericCast<int64_t>(appender_idx));
			return true;
		}
		return false;
	}

	void FlushAllFinishedAppenders() {
		while (FlushNextFinishedAppender()) {
		}
		if (!finished_appenders.empty()) {
			throw InternalException("Failed to flush TPC-DS dsdgen appenders in deterministic order");
		}
	}

	void Finish() {
		if (finished.load()) {
			return;
		}
		FlushAllFinishedAppenders();
		for (idx_t table_idx = CALL_CENTER; table_idx < append_info.size(); table_idx++) {
			if (append_info[table_idx]) {
				append_info[table_idx]->Close();
			}
		}
		generated_work.store(total_work);
		flushed_work.store(total_work);
		finished.store(true);
	}

private:
	ClientContext &context;
	double scale;
	Identifier catalog_name;
	Identifier schema;
	string suffix;
	unique_ptr<TPCDSDSDGenParameters> parameters;
	duckdb::vector<duckdb::unique_ptr<tpcds_append_information>> append_info;
	atomic<bool> finished {false};
	DSDGenMode mode = DSDGenMode::SEQUENTIAL;
	bool can_yield = true;
	idx_t total_work = 0;
	atomic<idx_t> generated_work {0};
	atomic<idx_t> flushed_work {0};
	idx_t row_batch_size = DSDGEN_MIN_ROW_BATCH_SIZE;

	idx_t parallel_thread_count = 1;
	vector<DSDGenWorkItem> parallel_work_items;
	idx_t parallel_work_offset = 0;
	vector<unique_ptr<FinishedDSDGenAppender>> finished_appenders;
	vector<int> parallel_next_flush_step;

	int table_id = CALL_CENTER;
	bool table_started = false;
	ds_key_t current_row = 1;
	ds_key_t current_table_remaining = 0;
	idx_t current_table_progress_units_per_row = 1;
	tpcds_builder_func builder_func = nullptr;
	bool sequential_thread_initialized = false;
	std::thread::id sequential_thread_id;
};

unique_ptr<DSDGenGenerator> CreateDSDGenGenerator(ClientContext &context, double sf, const Identifier &catalog,
                                                  const Identifier &schema, string suffix) {
	return make_uniq<TPCDSDSDGenGenerator>(context, sf, catalog, schema, std::move(suffix));
}

void DSDGenWrapper::DSDGen(double scale, ClientContext &context, const Identifier &catalog_name,
                           const Identifier &schema, string suffix) {
	auto generator = CreateDSDGenGenerator(context, scale, catalog_name, schema, std::move(suffix));
	while (!generator->GenerateNext()) {
	}
}

uint32_t DSDGenWrapper::QueriesCount() {
	return TPCDS_QUERIES_COUNT;
}

string DSDGenWrapper::GetQuery(int query) {
	if (query <= 0 || query > TPCDS_QUERIES_COUNT) {
		throw SyntaxException("Out of range TPC-DS query number %d", query);
	}
	return TPCDS_QUERIES[query - 1];
}

//! Q9 compares store_sales counts against thresholds that qgen draws from a range proportional to the store_sales
//! row count, i.e. to the scale factor; scale the stored SF 1 thresholds so the CASE branches keep their selectivity
static string ScaleQ9Thresholds(const string &query, double sf) {
	string result;
	idx_t pos = 0;
	idx_t scaled_count = 0;
	while (pos < query.size()) {
		auto start = query.find(") > ", pos);
		if (start == string::npos) {
			break;
		}
		start += 4;
		auto end = start;
		int64_t threshold = 0;
		while (end < query.size() && StringUtil::CharacterIsDigit(query[end])) {
			threshold = threshold * 10 + (query[end] - '0');
			end++;
		}
		result += query.substr(pos, start - pos);
		pos = end;
		if (end == start || query.compare(end, 5, " THEN") != 0) {
			result += query.substr(start, end - start);
			continue;
		}
		auto scaled = MaxValue<int64_t>(1, static_cast<int64_t>(std::llround(static_cast<double>(threshold) * sf)));
		result += to_string(scaled);
		scaled_count++;
	}
	result += query.substr(pos);
	D_ASSERT(scaled_count == 5);
	return result;
}

string DSDGenWrapper::GetQuery(int query, double sf) {
	if (Value::IsNan(sf) || sf <= 0 || sf > DSDGEN_MAX_SCALE_FACTOR) {
		throw InvalidInputException("TPC-DS queries require a scale factor between 0 and %d", DSDGEN_MAX_SCALE_FACTOR);
	}
	auto result = GetQuery(query);
	if (query != 9) {
		return result;
	}
	return ScaleQ9Thresholds(result, sf);
}

string DSDGenWrapper::GetAnswer(double sf, int query) {
	if (query <= 0 || query > TPCDS_QUERIES_COUNT) {
		throw SyntaxException("Out of range TPC-DS query number %d", query);
	}

	if (sf == 0.01) {
		return TPCDS_ANSWERS_SF0_01[query - 1];
	} else if (sf == 1) {
		return TPCDS_ANSWERS_SF1[query - 1];
	} else {
		throw NotImplementedException("Don't have TPC-DS answers for SF %llf!", sf);
	}
}

} // namespace tpcds
