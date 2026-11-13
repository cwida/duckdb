#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/data_table.hpp"

using namespace duckdb;

namespace {

struct TableScanSnapshotCheckpoint : public OptimizerExtensionInfo {
	explicit TableScanSnapshotCheckpoint(Connection &writer_p, bool copy_bind_data_p = false)
	    : writer(writer_p), copy_bind_data(copy_bind_data_p) {
	}

	Connection &writer;
	const bool copy_bind_data;
	bool checkpointed = false;

	static optional_ptr<LogicalGet> FindSnapshotScan(LogicalOperator &op) {
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = op.Cast<LogicalGet>();
			if (get.function.GetDefinition()->name == "seq_scan" && !get.scan_partition_indices.empty()) {
				return get;
			}
		}
		for (auto &child : op.children) {
			auto get = FindSnapshotScan(*child);
			if (get) {
				return get;
			}
		}
		return nullptr;
	}

	static void CheckpointAfterSnapshotCapture(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		auto &info = static_cast<TableScanSnapshotCheckpoint &>(*input.info);
		if (info.checkpointed) {
			return;
		}
		auto get = FindSnapshotScan(*plan);
		REQUIRE(get);
		auto &bind_data = get->bind_data->Cast<TableScanBindData>();
		auto &storage = bind_data.table.Cast<DuckTableEntry>().GetStorage();
		auto row_group_count = storage.GetRowGroupCount();
		vector<PartitionStatistics> before;
		if (info.copy_bind_data) {
			GetPartitionStatsInput stats_input(get->function, get->bind_data.get());
			before = get->function.get_partition_stats(input.context, stats_input);
			auto copy = get->bind_data->Copy();
			REQUIRE(get->bind_data->Equals(*copy));
			// Execute with the copy after destroying the original bind data.
			get->bind_data = std::move(copy);
		}

		// Change the row group layout after optimization, before scan initialization.
		info.checkpointed = true;
		if (info.copy_bind_data) {
			REQUIRE_NO_FAIL(info.writer.Query("INSERT INTO t SELECT i FROM range(6144, 8192) r(i)"));
		}
		// Use another connection because the reader's SELECT still holds its context lock.
		REQUIRE_NO_FAIL(info.writer.Query("CHECKPOINT"));
		REQUIRE(storage.GetRowGroupCount() != row_group_count);
		if (info.copy_bind_data) {
			GetPartitionStatsInput stats_input(get->function, get->bind_data.get());
			auto after = get->function.get_partition_stats(input.context, stats_input);
			REQUIRE(after.size() == before.size());
			for (idx_t i = 0; i < before.size(); i++) {
				REQUIRE(after[i].count == before[i].count);
				REQUIRE(after[i].row_start == before[i].row_start);
				REQUIRE(after[i].count_type == before[i].count_type);
			}
		}
	}
};

static shared_ptr<TableScanSnapshotCheckpoint> RegisterSnapshotCheckpoint(Connection &reader, Connection &writer,
                                                                          bool copy_bind_data = false) {
	auto checkpoint = make_shared_ptr<TableScanSnapshotCheckpoint>(writer, copy_bind_data);
	OptimizerExtension extension;
	extension.optimizer_info = checkpoint;
	extension.optimize_function = TableScanSnapshotCheckpoint::CheckpointAfterSnapshotCapture;
	ExtensionCallbackManager::Get(*reader.context).Register(std::move(extension));
	return checkpoint;
}

} // namespace

TEST_CASE("Table scan snapshot preserves partial aggregates across checkpoint", "[api][table_scan_snapshot]") {
	auto path = TestCreatePath("table_scan_snapshot_checkpoint.db");
	DeleteDatabase(path);
	DuckDB db(nullptr);
	Connection reader(db);
	Connection writer(db);
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS scan_db (ROW_GROUP_SIZE 2048)"));
	REQUIRE_NO_FAIL(reader.Query("USE scan_db"));
	REQUIRE_NO_FAIL(writer.Query("USE scan_db"));
	REQUIRE_NO_FAIL(reader.Query("SET wal_autocheckpoint='1TB'"));
	REQUIRE_NO_FAIL(reader.Query("CREATE TABLE t AS SELECT i FROM range(8192) r(i)"));
	REQUIRE_NO_FAIL(reader.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(reader.Query("DELETE FROM t WHERE i < 2048"));

	auto checkpoint = RegisterSnapshotCheckpoint(reader, writer);
	auto result = reader.Query("SELECT count(*), min(i), max(i) FROM t WHERE i > 2500");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(checkpoint->checkpointed);
	REQUIRE(CHECK_COLUMN(result, 0, {5691}));
	REQUIRE(CHECK_COLUMN(result, 1, {2501}));
	REQUIRE(CHECK_COLUMN(result, 2, {8191}));
}

TEST_CASE("Table scan snapshot survives checkpoint and bind data copies", "[api][table_scan_snapshot]") {
	auto path = TestCreatePath("table_scan_snapshot_copy.db");
	DeleteDatabase(path);
	DuckDB db(nullptr);
	Connection reader(db);
	Connection writer(db);
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS scan_db (ROW_GROUP_SIZE 2048)"));
	REQUIRE_NO_FAIL(reader.Query("USE scan_db"));
	REQUIRE_NO_FAIL(writer.Query("USE scan_db"));
	REQUIRE_NO_FAIL(reader.Query("CREATE TABLE t AS SELECT i FROM range(6144) r(i)"));
	REQUIRE_NO_FAIL(reader.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	REQUIRE_NO_FAIL(reader.Query("INSERT INTO t VALUES (10000), (10001)"));

	auto checkpoint = RegisterSnapshotCheckpoint(reader, writer, true);
	auto result = reader.Query("SELECT count(*), min(i), max(i) FROM t WHERE i > 2500");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(checkpoint->checkpointed);
	REQUIRE(CHECK_COLUMN(result, 0, {3645}));
	REQUIRE(CHECK_COLUMN(result, 1, {2501}));
	REQUIRE(CHECK_COLUMN(result, 2, {10001}));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));
}
