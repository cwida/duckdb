#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "catch.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/vector.hpp"

#include "test_helpers.hpp"

#include <unordered_set>
#include <thread>

using namespace duckdb;

enum class AttachTaskType { CREATE_TABLE, LOOKUP, APPEND, APPLY_CHANGES, DESCRIBE_TABLE, CHECKPOINT };

namespace {

string AttachTaskTypeToString(AttachTaskType task_type) {
	switch (task_type) {
	case AttachTaskType::CREATE_TABLE:
		return "CREATE";
	case AttachTaskType::LOOKUP:
		return "LOOKUP";
	case AttachTaskType::APPEND:
		return "APPEND";
	case AttachTaskType::APPLY_CHANGES:
		return "UPSERT";
	case AttachTaskType::DESCRIBE_TABLE:
		return "DESCRIBE";
	case AttachTaskType::CHECKPOINT:
		return "CHECKPOINT";
	default:
		return "UNKNOWN";
	}
}

string test_dir_path;
const string prefix = "db_";
const string suffix = ".db";

string getDBPath(idx_t i) {
	return test_dir_path + "/" + prefix + to_string(i) + suffix;
}

string getDBName(idx_t i) {
	return prefix + to_string(i);
}

const idx_t db_count = 10;
const idx_t worker_count = 40;
const idx_t iteration_count = 100;
const idx_t nr_initial_rows = 2050;

vector<vector<string>> logging;
atomic<bool> success {true};

unique_ptr<QueryResult> execQuery(Connection &conn, const string &query) {
	auto result = conn.Query(query);
	if (result->HasError()) {
		auto err = result->GetError();
		Printer::PrintF("Failed to execute query %s:\n------\n%s\n-------", query, err);
		success = false;
	}
	return result;
}

struct TableInfo {
	idx_t size;
};

struct DBInfo {
	mutex mu;
	idx_t table_count = 0;
	vector<TableInfo> tables;
};

struct AttachTask {
	AttachTaskType type;
	optional_idx db_id;
	optional_idx tbl_id;
	optional_idx tbl_size;
	vector<idx_t> ids;
	bool actual_describe = false;
};

struct AttachWorker;

class DBPoolMgr {
public:
	mutex mu;
	map<idx_t, idx_t> m;

	void addWorker(AttachWorker &worker, const idx_t i);
	void removeWorker(AttachWorker &worker, const idx_t i);

	DBInfo db_infos[db_count];
};

struct AttachWorker {
public:
	AttachWorker(DuckDB &db, idx_t worker_id, vector<string> &logs, DBPoolMgr &db_pool)
	    : conn(db), worker_id(worker_id), logs(logs), db_pool(db_pool) {
	}

public:
	unique_ptr<QueryResult> execQuery(const string &query) {
		return ::execQuery(conn, query);
	}
	void Work();

private:
	AttachTask RandomTask();
	void createTbl(AttachTask &task);
	void lookup(AttachTask &task);
	void append_internal(AttachTask &task, const bool is_upsert);
	void append(AttachTask &task);
	void apply_changes(AttachTask &task);
	void describe_tbl(AttachTask &task);
	void checkpoint_db(AttachTask &task);
	void GetRandomTable(AttachTask &task);
	void addLog(const string &msg) {
		logs.push_back(msg);
	}

public:
	Connection conn;
	idx_t worker_id;
	vector<string> &logs;
	DBPoolMgr &db_pool;
};

void DBPoolMgr::addWorker(AttachWorker &worker, const idx_t i) {
	lock_guard<mutex> lock(mu);

	if (m.find(i) != m.end()) {
		m[i]++;
		return;
	}
	m[i] = 1;

	string query = "ATTACH '" + getDBPath(i) + "'";
	worker.execQuery(query);
}

void DBPoolMgr::removeWorker(AttachWorker &worker, const idx_t i) {
	lock_guard<mutex> lock(mu);

	m[i]--;
	if (m[i] != 0) {
		return;
	}

	m.erase(i);
	string query = "DETACH " + getDBName(i);
	worker.execQuery(query);
}

void AttachWorker::createTbl(AttachTask &task) {
	auto db_id = task.db_id.GetIndex();
	auto &db_infos = db_pool.db_infos;
	lock_guard<mutex> lock(db_infos[db_id].mu);
	auto tbl_id = db_infos[db_id].table_count;
	db_infos[db_id].tables.emplace_back(TableInfo {nr_initial_rows});
	db_infos[db_id].table_count++;

	string tbl_path = StringUtil::Format("%s.tbl_%d", getDBName(db_id), tbl_id);
	string create_sql = StringUtil::Format(
	    "CREATE TABLE %s(i BIGINT, s VARCHAR, ts TIMESTAMP, obj STRUCT(key1 UBIGINT, key2 VARCHAR))", tbl_path);
	addLog("; q: " + create_sql);
	execQuery(create_sql);
	string insert_sql = "INSERT INTO " + tbl_path +
	                    " SELECT "
	                    "range::UBIGINT AS i, "
	                    "range::VARCHAR AS s, "
	                    // Note: We increment timestamps by 1 millisecond (i.e., 1000 microseconds).
	                    "epoch_ms(range) AS ts, "
	                    "{'key1': range::UBIGINT, 'key2': range::VARCHAR} AS obj "
	                    "FROM range(" +
	                    to_string(nr_initial_rows) + ")";
	addLog("; q: " + insert_sql);
	execQuery(insert_sql);
}

void AttachWorker::lookup(AttachTask &task) {
	if (!task.tbl_id.IsValid()) {
		return;
	}
	auto db_id = task.db_id.GetIndex();
	auto tbl_id = task.tbl_id.GetIndex();
	auto expected_max_val = task.tbl_size.GetIndex() - 1;

	// Run the query.
	auto table_name = getDBName(db_id) + ".tbl_" + to_string(tbl_id);
	string query = "SELECT i, s, ts, obj FROM " + table_name + " WHERE i = " + to_string(expected_max_val);
	addLog("q: " + query);
	auto result = execQuery(query);
	if (!result) {
		addLog("FAILURE - Unexpected empty result");
		success = false;
		return;
	}
	if (result->RowCount() == 0) {
		addLog("FAILURE - No rows returned from query");
		success = false;
		return;
	}
	if (!CHECK_COLUMN(result, 0, {Value::UBIGINT(expected_max_val)})) {
		success = false;
		return;
	}
	if (!CHECK_COLUMN(result, 1, {to_string(expected_max_val)})) {
		success = false;
		return;
	}
	if (!CHECK_COLUMN(result, 2, {Value::TIMESTAMP(timestamp_t {static_cast<int64_t>(expected_max_val * 1000)})})) {
		success = false;
		return;
	}
	if (!CHECK_COLUMN(
	        result, 3,
	        {Value::STRUCT({{"key1", Value::UBIGINT(expected_max_val)}, {"key2", to_string(expected_max_val)}})})) {
		success = false;
	}
}

void AttachWorker::append_internal(AttachTask &task, bool is_upsert) {
	auto db_id = task.db_id.GetIndex();
	auto tbl_id = task.tbl_id.GetIndex();
	auto tbl_str = "tbl_" + to_string(tbl_id);
	// set appender
	addLog("db: " + getDBName(db_id) + "; table: " + tbl_str + "; append rows");

	try {
		// QueryAppender
		child_list_t<LogicalType> struct_children;
		struct_children.emplace_back(make_pair("key1", LogicalTypeId::UBIGINT));
		struct_children.emplace_back(make_pair("key2", LogicalTypeId::VARCHAR));

		const vector<LogicalType> types = {LogicalType::UBIGINT, LogicalType::VARCHAR, LogicalType::TIMESTAMP,
		                                   LogicalType::STRUCT(struct_children)};
		unique_ptr<BaseAppender> base_appender;
		if (is_upsert) {
			auto query = StringUtil::Format("MERGE INTO %s.main.%s USING appended_data USING (i) WHEN MATCHED THEN "
			                                "UPDATE WHEN NOT MATCHED THEN INSERT",
			                                SQLIdentifier(getDBName(db_id)), SQLIdentifier(tbl_str));
			vector<Identifier> names;
			names.push_back("i");
			names.push_back("s");
			names.push_back("ts");
			names.push_back("obj");
			base_appender = make_uniq<QueryAppender>(conn, query, types, names);
		} else {
			base_appender = make_uniq<Appender>(conn, Identifier(getDBName(db_id)), Identifier::DefaultSchema(),
			                                    Identifier(tbl_str));
		}
		auto &appender = *base_appender;

		// Fill the data chunk.
		DataChunk chunk;
		chunk.Initialize(*conn.context, types);

		// int
		auto &col_ubigint = chunk.data[0];
		auto data_ubigint = FlatVector::GetDataMutable<uint64_t>(col_ubigint);
		// varchar
		auto &col_varchar = chunk.data[1];
		auto data_varchar = FlatVector::GetDataMutable<string_t>(col_varchar);
		// timestamp
		auto &col_ts = chunk.data[2];
		auto data_ts = FlatVector::GetDataMutable<timestamp_t>(col_ts);
		// struct
		auto &col_struct = chunk.data[3];
		auto &data_struct_entries = StructVector::GetEntries(col_struct);
		auto &entry_ubigint = data_struct_entries[0];
		auto data_struct_ubigint = FlatVector::GetDataMutable<uint64_t>(entry_ubigint);
		auto &entry_varchar = data_struct_entries[1];
		auto data_struct_varchar = FlatVector::GetDataMutable<string_t>(entry_varchar);

		for (idx_t i = 0; i < task.ids.size(); i++) {
			auto row_idx = task.ids[i];
			data_ubigint[i] = row_idx;
			data_varchar[i] = StringVector::AddString(col_varchar, to_string(row_idx));
			data_ts[i] = timestamp_t {static_cast<int64_t>(1000 * (row_idx))};
			data_struct_ubigint[i] = row_idx;
			data_struct_varchar[i] = StringVector::AddString(entry_varchar, to_string(row_idx));
		}

		chunk.SetChildCardinality(task.ids.size());
		appender.AppendDataChunk(chunk);
		appender.Close();

	} catch (const std::exception &e) {
		addLog("Caught exception when using Appender: " + string(e.what()));
		success = false;
	} catch (...) {
		addLog("Caught error when using Appender!");
		success = false;
	}
}

void AttachWorker::append(AttachTask &task) {
	if (!task.tbl_id.IsValid()) {
		return;
	}
	auto db_id = task.db_id.GetIndex();
	auto tbl_id = task.tbl_id.GetIndex();
	auto &db_infos = db_pool.db_infos;
	lock_guard<mutex> lock(db_infos[db_id].mu);
	auto current_num_rows = db_infos[db_id].tables[tbl_id].size;
	idx_t append_count = STANDARD_VECTOR_SIZE;

	for (idx_t i = 0; i < append_count; i++) {
		task.ids.push_back(current_num_rows + i);
	}

	append_internal(task, false);
	db_infos[db_id].tables[tbl_id].size += append_count;
}

void AttachWorker::apply_changes(AttachTask &task) {
	if (!task.tbl_id.IsValid()) {
		return;
	}
	auto db_id = task.db_id.GetIndex();
	auto &db_infos = db_pool.db_infos;
	lock_guard<mutex> lock(db_infos[db_id].mu);
	execQuery("BEGIN");
	append_internal(task, true);
	execQuery("COMMIT");
}

void AttachWorker::describe_tbl(AttachTask &task) {
	if (!task.tbl_id.IsValid()) {
		return;
	}
	auto db_id = task.db_id.GetIndex();
	auto tbl_id = task.tbl_id.GetIndex();
	auto tbl_str = "tbl_" + to_string(tbl_id);
	auto actual_describe = task.actual_describe;
	string describe_sql;
	if (actual_describe) {
		describe_sql = StringUtil::Format("DESCRIBE %s.%s.%s", getDBName(db_id), DEFAULT_SCHEMA, tbl_str);
	} else {
		describe_sql = StringUtil::Format("SELECT 1 FROM %s.%s.%s LIMIT 1", getDBName(db_id), DEFAULT_SCHEMA, tbl_str);
	}

	addLog("q: " + describe_sql);
	execQuery(describe_sql);
}

void AttachWorker::checkpoint_db(AttachTask &task) {
	auto db_id = task.db_id.GetIndex();
	auto &db_infos = db_pool.db_infos;
	unique_lock<mutex> lock(db_infos[db_id].mu);
	string checkpoint_sql = "CHECKPOINT " + getDBName(db_id);
	addLog("q: " + checkpoint_sql);
	// checkpoint can fail, we don't care
	conn.Query(checkpoint_sql);
}

void AttachWorker::GetRandomTable(AttachTask &task) {
	auto &db_infos = db_pool.db_infos;
	auto db_id = task.db_id.GetIndex();
	lock_guard<mutex> lock(db_infos[db_id].mu);
	auto max_tbl_id = db_infos[db_id].table_count;
	if (max_tbl_id == 0) {
		return;
	}

	task.tbl_id = std::rand() % max_tbl_id;
	task.tbl_size = db_infos[db_id].tables[task.tbl_id.GetIndex()].size;
}

AttachTask AttachWorker::RandomTask() {
	AttachTask result;
	idx_t scenario_id = std::rand() % 10;
	result.db_id = std::rand() % db_count;
	switch (scenario_id) {
	case 0:
		result.type = AttachTaskType::CREATE_TABLE;
		GetRandomTable(result);
		break;
	case 1:
		result.type = AttachTaskType::LOOKUP;
		GetRandomTable(result);
		break;
	case 2:
		result.type = AttachTaskType::APPEND;
		GetRandomTable(result);
		break;
	case 3:
		result.type = AttachTaskType::APPLY_CHANGES;
		GetRandomTable(result);
		if (result.tbl_id.IsValid()) {
			auto current_num_rows = result.tbl_size.GetIndex();
			idx_t modulo = STANDARD_VECTOR_SIZE < 3 ? STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE / 3;
			idx_t delete_count = std::rand() % modulo;
			if (delete_count == 0) {
				delete_count = 1;
			}

			unordered_set<idx_t> unique_ids;
			for (idx_t i = 0; i < delete_count; i++) {
				unique_ids.insert(std::rand() % current_num_rows);
			}
			for (auto &id : unique_ids) {
				result.ids.push_back(id);
			}
		}
		break;
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
		result.type = AttachTaskType::DESCRIBE_TABLE;
		GetRandomTable(result);
		result.actual_describe = std::rand() % 2 == 0;
		break;
	default:
		result.type = AttachTaskType::CHECKPOINT;
		break;
	}
	return result;
}

void AttachWorker::Work() {
	Profiler profiler;
	AttachTask slowest_task;

	for (idx_t i = 0; i < iteration_count; i++) {
		if (!success) {
			return;
		}

		try {
			auto task = RandomTask();
			db_pool.addWorker(*this, task.db_id.GetIndex());

			profiler.Start();
			switch (task.type) {
			case AttachTaskType::CREATE_TABLE:
				createTbl(task);
				break;
			case AttachTaskType::LOOKUP:
				lookup(task);
				break;
			case AttachTaskType::APPEND:
				append(task);
				break;
			case AttachTaskType::APPLY_CHANGES:
				apply_changes(task);
				break;
			case AttachTaskType::DESCRIBE_TABLE:
				describe_tbl(task);
				break;
			case AttachTaskType::CHECKPOINT:
				checkpoint_db(task);
				break;
			default:
				addLog("invalid task type");
				success = false;
				return;
			}
			profiler.End();
			db_pool.removeWorker(*this, task.db_id.GetIndex());
			auto elapsed = profiler.Elapsed();

			// NOTE: Magic threshold used for debugging slowness in this test.
			// NOTE Set to a fairly high value for CI purposes.
			if (elapsed >= 1) {
				Printer::PrintF("Slow task %s - took %lf seconds\n", AttachTaskTypeToString(task.type), elapsed);
			}

		} catch (const std::exception &e) {
			addLog("Caught exception when running iterations: " + string(e.what()));
			success = false;
			return;
		} catch (...) {
			addLog("Caught unknown when using running iterations");
			success = false;
			return;
		}
	}
}

void workUnit(std::unique_ptr<AttachWorker> worker) {
	worker->Work();
}

TEST_CASE("Run a concurrent ATTACH/DETACH scenario", "[interquery][.]") {
	test_dir_path = TestDirectoryPath();
	DBPoolMgr db_pool;
	DuckDB db(nullptr);
	Connection init_conn(db);

	execQuery(init_conn, "SET catalog_error_max_schemas = '0'");
	execQuery(init_conn, "SET threads = '1'");
	execQuery(init_conn, "SET storage_compatibility_version = 'latest'");
	execQuery(init_conn, "CALL enable_logging()");
	execQuery(init_conn, "PRAGMA enable_profiling='no_output'");

	logging.resize(worker_count);
	vector<std::thread> workers;
	for (idx_t i = 0; i < worker_count; i++) {
		auto worker = make_uniq<AttachWorker>(db, i, logging[i], db_pool);
		workers.emplace_back(workUnit, std::move(worker));
	}

	for (auto &worker : workers) {
		worker.join();
	}
	if (!success) {
		for (idx_t worker_id = 0; worker_id < logging.size(); worker_id++) {
			for (auto &log : logging[worker_id]) {
				Printer::PrintF("thread %d; %s", worker_id, log);
			}
		}
		FAIL();
	}
	ClearTestDirectory();
}

} // anonymous namespace
