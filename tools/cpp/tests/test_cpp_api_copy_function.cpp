#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Stable C++ API tests: CopyFunction. The extension-registration path is
// covered by the static extension demo (test/api/capi/v2/static_extension);
// these cover the connection path.
//
// Callbacks latch what they observe into file-scope statics rather than
// asserting in place: a failed REQUIRE inside a callback would surface as a
// query error rather than as the assertion itself.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// The COPY statements driving a format.
std::string CopyToStatement(const std::string &source, const std::string &path, const std::string &format,
                            const std::string &options = "") {
	return "COPY (" + source + ") TO '" + path + "' (FORMAT " + format + ", USE_TMP_FILE FALSE" + options + ")";
}

std::string CopyFromStatement(const std::string &table, const std::string &path, const std::string &format,
                              const std::string &options = "") {
	return "COPY " + table + " FROM '" + path + "' (FORMAT " + format + options + ")";
}

std::string ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

// Collect a single BIGINT column, asserting every row valid.
std::vector<int64_t> CollectBigints(QueryResult result) {
	std::vector<int64_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int64_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// cpp_copy (COPY TO): the bind data records the column count, the init data
// owns the output path and accumulates the rows flushed to it, and each batch
// is kept as its own batch data until the flush counts its rows. Finalize
// writes "<columns> <rows>" to the target through the engine's file system.
// ---------------------------------------------------------------------------

struct FileState {
	std::string path;
	idx_t columns = 0;
	idx_t total_rows = 0;
};

struct {
	idx_t column_count = 0;
	std::string column_name;
	LogicalTypeId column_type = LogicalTypeId::INVALID;
	std::string options;
	std::atomic<idx_t> batches {0};

	void Reset() {
		column_count = 0;
		column_name.clear();
		column_type = LogicalTypeId::INVALID;
		options.clear();
		batches = 0;
	}
} summary_seen;

void SummaryBind(CopyFunction::CopyToBindInput &input) {
	summary_seen.column_count = input.GetColumnCount();
	summary_seen.column_name = input.GetColumnName(0);
	summary_seen.column_type = input.GetColumnType(0).GetTypeId();
	for (idx_t i = 0; i < input.GetOptionCount(); i++) {
		summary_seen.options += input.GetOptionName(i) + "=" + input.GetOptionValue(i).ToText() + "\n";
	}
	input.SetBindData<idx_t>(input.GetColumnCount());
}

// Asks for batches larger than the input: the whole copy arrives as one batch per thread.
void SummaryBatchSize(CopyFunction::CopyToBatchSizeInput &input) {
	// The batch size callback runs after bind and sees its data.
	if (input.GetBindData<idx_t>() != 1) {
		throw InvalidInputException("batch size callback did not see the bind data");
	}
	input.SetTarget(100000);
}

void SummaryInit(CopyFunction::CopyToInitInput &input) {
	input.SetInitData<FileState>(FileState {input.GetFilePath(), input.GetBindData<idx_t>(), 0});
}

void SummaryBatch(CopyFunction::CopyToBatchInput &input) {
	summary_seen.batches++;
	// The batch is ours: hand it over to the flush as is.
	input.SetBatchData<ColumnDataCollection>(input.TakeBatch());
}

void SummaryFlush(CopyFunction::CopyToFlushInput &input) {
	input.GetInitData<FileState>().total_rows += input.GetBatchData<ColumnDataCollection>().GetRowCount();
}

void SummaryFinalize(CopyFunction::CopyToFinalizeInput &input) {
	const auto &state = input.GetInitData<FileState>();
	const auto contents = std::to_string(state.columns) + " " + std::to_string(state.total_rows);

	auto fs = input.GetContext().GetFileSystem();
	auto file = fs.OpenFile(state.path, {FileFlags::WRITE, FileFlags::FILE_CREATE});
	file.Write(contents.data(), contents.size());
}

// ---------------------------------------------------------------------------
// cpp_reader (COPY FROM): produces the target table's single BIGINT column as
// 0, 1, ... ROWS-1, claiming slices of rows from a shared cursor.
// ---------------------------------------------------------------------------

struct ReaderBind {
	int64_t rows = 0;
	std::string path;
	std::string options;
};

struct ReaderGlobal {
	std::atomic<int64_t> position {0};
};

struct ReaderLocal {
	int64_t produced = 0;
};

struct {
	std::string path;
	std::string options;
	std::string column_name;
	LogicalTypeId column_type = LogicalTypeId::INVALID;
	std::atomic<idx_t> execs {0};
	std::atomic<idx_t> locals {0};

	void Reset() {
		path.clear();
		options.clear();
		column_name.clear();
		column_type = LogicalTypeId::INVALID;
		execs = 0;
		locals = 0;
	}
} reader_seen;

// How many rows a batch produces. The API exposes no output-chunk capacity, so the tests stay within the smallest
// vector size the build can be configured with.
constexpr int64_t READER_BATCH_ROWS = 2;

void ReaderBind_(CopyFunction::CopyFromBindInput &input) {
	ReaderBind bind;
	bind.path = input.GetFilePath();
	for (idx_t i = 0; i < input.GetOptionCount(); i++) {
		const auto name = input.GetOptionName(i);
		auto value = input.GetOptionValue(i);
		bind.options += name + "=" + value.ToText() + "\n";
		if (name == "rows") {
			bind.rows = value.Get<int32_t>();
		}
	}
	reader_seen.path = bind.path;
	reader_seen.options = bind.options;
	reader_seen.column_name = input.GetColumnName(0);
	reader_seen.column_type = input.GetColumnType(0).GetTypeId();
	input.SetCardinality(static_cast<idx_t>(bind.rows), true);
	input.SetBindData<ReaderBind>(std::move(bind));
}

void ReaderInitGlobal(CopyFunction::CopyFromInitGlobalInput &input) {
	input.SetGlobalState<ReaderGlobal>();
	input.SetMaxThreads(2);
}

void ReaderInitLocal(CopyFunction::CopyFromInitLocalInput &input) {
	reader_seen.locals++;
	input.SetLocalState<ReaderLocal>();
}

void ReaderExec(CopyFunction::CopyFromExecInput &input) {
	reader_seen.execs++;
	const auto &bind = input.GetBindData<ReaderBind>();
	auto &global = input.GetGlobalState<ReaderGlobal>();
	auto &local = input.GetLocalState<ReaderLocal>();

	auto chunk = input.GetOutputChunk();
	auto vec = chunk.GetVector(0);
	auto *out = vec.GetDataMutable<int64_t>();

	const auto start = global.position.fetch_add(READER_BATCH_ROWS);
	auto produced = bind.rows - start;
	if (produced > READER_BATCH_ROWS) {
		produced = READER_BATCH_ROWS;
	}
	if (produced < 0) {
		produced = 0;
	}
	for (int64_t i = 0; i < produced; i++) {
		out[i] = start + i;
	}
	local.produced += produced;

	// The first vector's size is the batch's row count; 0 ends the read.
	vec.SetSize(static_cast<idx_t>(produced));
}

void ReaderProgress(CopyFunction::CopyFromProgressInput &input) {
	const auto &bind = input.GetBindData<ReaderBind>();
	const auto &global = input.GetGlobalState<ReaderGlobal>();
	input.SetProgress(bind.rows > 0 ? static_cast<double>(global.position) / static_cast<double>(bind.rows) : 1.0);
}

// Data-flow probes: each callback latches the user data it was handed.
struct {
	std::atomic<const std::string *> in_bind {nullptr};
	std::atomic<const std::string *> in_init {nullptr};
	std::atomic<const std::string *> in_batch {nullptr};
	std::atomic<const std::string *> in_flush {nullptr};
	std::atomic<const std::string *> in_finalize {nullptr};
	std::atomic<const std::string *> in_from_bind {nullptr};
	std::atomic<const std::string *> in_from_exec {nullptr};

	void Reset() {
		in_bind = nullptr;
		in_init = nullptr;
		in_batch = nullptr;
		in_flush = nullptr;
		in_finalize = nullptr;
		in_from_bind = nullptr;
		in_from_exec = nullptr;
	}
} user_data_seen;

void UserDataBind(CopyFunction::CopyToBindInput &input) {
	user_data_seen.in_bind = &input.GetUserData<std::string>();
}
void UserDataInit(CopyFunction::CopyToInitInput &input) {
	user_data_seen.in_init = &input.GetUserData<std::string>();
}
void UserDataBatch(CopyFunction::CopyToBatchInput &input) {
	user_data_seen.in_batch = &input.GetUserData<std::string>();
}
void UserDataFlush(CopyFunction::CopyToFlushInput &input) {
	user_data_seen.in_flush = &input.GetUserData<std::string>();
}
void UserDataFinalize(CopyFunction::CopyToFinalizeInput &input) {
	user_data_seen.in_finalize = &input.GetUserData<std::string>();
}
void UserDataFromBind(CopyFunction::CopyFromBindInput &input) {
	user_data_seen.in_from_bind = &input.GetUserData<std::string>();
}
void UserDataFromExec(CopyFunction::CopyFromExecInput &input) {
	user_data_seen.in_from_exec = &input.GetUserData<std::string>();
	// Produce nothing: the read ends at once.
}

// Reads a slot that was never set: the wrapper refuses instead of handing out null.
void NoUserDataBind(CopyFunction::CopyToBindInput &input) {
	input.GetUserData<int>();
}
void NoBindDataInit(CopyFunction::CopyToInitInput &input) {
	input.GetBindData<int>();
}
void NoInitDataBatch(CopyFunction::CopyToBatchInput &input) {
	input.GetInitData<int>();
}
void NoBatchDataFlush(CopyFunction::CopyToFlushInput &input) {
	input.GetBatchData<int>();
}
void NoGlobalStateExec(CopyFunction::CopyFromExecInput &input) {
	input.GetGlobalState<int>();
}

void NoopBind(CopyFunction::CopyToBindInput &) {
}
void NoopInit(CopyFunction::CopyToInitInput &) {
}
void NoopBatch(CopyFunction::CopyToBatchInput &) {
}
void NoopFlush(CopyFunction::CopyToFlushInput &) {
}
void NoopFromBind(CopyFunction::CopyFromBindInput &) {
}
void NoopFromExec(CopyFunction::CopyFromExecInput &) {
}

void FailingBatch(CopyFunction::CopyToBatchInput &) {
	throw InvalidInputException("copy batch failed on purpose");
}

void FailingFromExec(CopyFunction::CopyFromExecInput &) {
	throw InvalidInputException("copy from exec failed on purpose");
}

// A batch size callback that sets a target of 0 fails the statement.
void ZeroBatchSize(CopyFunction::CopyToBatchSizeInput &input) {
	input.SetTarget(0);
}

// The batch can only be taken once.
void TakeTwiceBatch(CopyFunction::CopyToBatchInput &input) {
	auto first = input.TakeBatch();
	input.TakeBatch();
}

} // namespace

// ---------------------------------------------------------------------------
// COPY TO: register on a connection and drive through COPY ... TO.
// ---------------------------------------------------------------------------

TEST_CASE("Stable C++API: copy function writes through COPY TO", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// One sink thread and a batch size beyond the input: the whole copy arrives as a single batch.
	conn.Execute("SET threads = 1").Drain();
	auto function = CopyFunction::Create(conn.GetFactory());
	function.SetName("cpp_copy")
	    .SetCopyToBindCallback(SummaryBind)
	    .SetCopyToBatchSizeCallback(SummaryBatchSize)
	    .SetCopyToInitCallback(SummaryInit)
	    .SetCopyToBatchCallback(SummaryBatch)
	    .SetCopyToFlushCallback(SummaryFlush)
	    .SetCopyToFinalizeCallback(SummaryFinalize);
	conn.Register(function);

	summary_seen.Reset();
	const auto path = duckdb::TestCreatePath("cpp_copy_summary.txt");
	// COPY reports the number of rows written.
	REQUIRE(conn.Execute(CopyToStatement("SELECT r AS i FROM range(5000) t(r)", path, "cpp_copy", ", TAG 'x', FLAG"))
	            .Drain() == 5000);

	// Bind saw the SELECT's single BIGINT column and the function's own options.
	REQUIRE(summary_seen.column_count == 1);
	REQUIRE(summary_seen.column_name == "i");
	REQUIRE(summary_seen.column_type == LogicalTypeId::BIGINT);
	REQUIRE(summary_seen.options == "flag=true\ntag=x\n");
	REQUIRE(summary_seen.batches.load() == 1);

	// The bind -> init -> batch -> flush -> finalize chain observed 1 column and 5000 rows.
	REQUIRE(ReadFile(path) == "1 5000");
}

TEST_CASE("Stable C++API: copy function names its files with its extension", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("SET threads = 1").Drain();
	auto function = CopyFunction::Create(conn.GetFactory());
	REQUIRE_THROWS_MATCHES(function.SetFileExtension(".summary"), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	function.SetName("cpp_copy_ext")
	    .SetFileExtension("summary")
	    .SetCopyToBindCallback(SummaryBind)
	    .SetCopyToInitCallback(SummaryInit)
	    .SetCopyToBatchCallback(SummaryBatch)
	    .SetCopyToFlushCallback(SummaryFlush)
	    .SetCopyToFinalizeCallback(SummaryFinalize);
	conn.Register(function);

	// With PER_THREAD_OUTPUT DuckDB names the files in the directory itself.
	const auto dir = duckdb::TestCreatePath("cpp_copy_ext_dir");
	REQUIRE(conn.Execute("COPY (SELECT r AS i FROM range(10) t(r)) TO '" + dir +
	                     "' (FORMAT cpp_copy_ext, PER_THREAD_OUTPUT)")
	            .Drain() == 10);
	auto files = conn.GetFileSystem().Glob(dir + "/*");
	REQUIRE(files.size() == 1);
	REQUIRE(duckdb::StringUtil::EndsWith(files[0], ".summary"));
	REQUIRE(ReadFile(files[0]) == "1 10");
}

// ---------------------------------------------------------------------------
// COPY FROM: register on a connection and drive through COPY ... FROM.
// ---------------------------------------------------------------------------

TEST_CASE("Stable C++API: copy function reads through COPY FROM", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE target(i BIGINT)").Drain();

	auto function = CopyFunction::Create(conn.GetFactory());
	function.SetName("cpp_reader")
	    .SetCopyFromBindCallback(ReaderBind_)
	    .SetCopyFromInitGlobalCallback(ReaderInitGlobal)
	    .SetCopyFromInitLocalCallback(ReaderInitLocal)
	    .SetCopyFromExecCallback(ReaderExec)
	    .SetCopyFromProgressCallback(ReaderProgress);
	conn.Register(function);

	reader_seen.Reset();
	REQUIRE(conn.Execute(CopyFromStatement("target", "some/where.cpp", "cpp_reader", ", ROWS 25, PAIR (1, 'two')"))
	            .Drain() == 25);

	// Bind saw the path as written, the target's column and the options; a parenthesized option is a tuple.
	REQUIRE(reader_seen.path == "some/where.cpp");
	REQUIRE(reader_seen.column_name == "i");
	REQUIRE(reader_seen.column_type == LogicalTypeId::BIGINT);
	REQUIRE(reader_seen.options == "pair=(1, two)\nrows=25\n");
	REQUIRE(reader_seen.locals.load() >= 1);
	REQUIRE(reader_seen.execs.load() >= 1);

	// Every row landed in the table.
	auto rows = CollectBigints(conn.Execute("SELECT i FROM target ORDER BY i"));
	REQUIRE(rows.size() == 25);
	for (idx_t i = 0; i < rows.size(); i++) {
		REQUIRE(rows[i] == static_cast<int64_t>(i));
	}

	// A COPY FROM only function cannot be written to.
	REQUIRE_THROWS(conn.Execute(CopyToStatement("SELECT 1", "nowhere", "cpp_reader")).Drain());
}

// ---------------------------------------------------------------------------
// Data flow: user data reaches every phase; a slot that was never set is refused.
// ---------------------------------------------------------------------------

TEST_CASE("Stable C++API: copy function data flows through the phases", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE target(i BIGINT)").Drain();
	const auto path = duckdb::TestCreatePath("cpp_copy_user_data.txt");

	auto function = CopyFunction::Create(conn.GetFactory());
	function.SetName("cpp_copy_user_data")
	    .SetUserData<std::string>("copy user data")
	    .SetCopyToBindCallback(UserDataBind)
	    .SetCopyToInitCallback(UserDataInit)
	    .SetCopyToBatchCallback(UserDataBatch)
	    .SetCopyToFlushCallback(UserDataFlush)
	    .SetCopyToFinalizeCallback(UserDataFinalize)
	    .SetCopyFromBindCallback(UserDataFromBind)
	    .SetCopyFromExecCallback(UserDataFromExec);
	conn.Register(function);

	user_data_seen.Reset();
	conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_user_data")).Drain();
	conn.Execute(CopyFromStatement("target", path, "cpp_copy_user_data")).Drain();

	// One object, visible in every phase of both sides.
	const std::string *seen = user_data_seen.in_bind;
	REQUIRE(seen != nullptr);
	REQUIRE(*seen == "copy user data");
	REQUIRE(user_data_seen.in_init.load() == seen);
	REQUIRE(user_data_seen.in_batch.load() == seen);
	REQUIRE(user_data_seen.in_flush.load() == seen);
	REQUIRE(user_data_seen.in_finalize.load() == seen);
	REQUIRE(user_data_seen.in_from_bind.load() == seen);
	REQUIRE(user_data_seen.in_from_exec.load() == seen);

	// A slot that was never set is a clear error, not a null deref, from whichever phase reads it.
	auto no_user_data = CopyFunction::Create(conn.GetFactory());
	no_user_data.SetName("cpp_copy_no_user_data")
	    .SetCopyToBindCallback(NoUserDataBind)
	    .SetCopyToBatchCallback(NoopBatch)
	    .SetCopyToFlushCallback(NoopFlush);
	conn.Register(no_user_data);
	REQUIRE_THROWS_MATCHES(conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_no_user_data")),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto no_bind_data = CopyFunction::Create(conn.GetFactory());
	no_bind_data.SetName("cpp_copy_no_bind_data")
	    .SetCopyToBindCallback(NoopBind)
	    .SetCopyToInitCallback(NoBindDataInit)
	    .SetCopyToBatchCallback(NoopBatch)
	    .SetCopyToFlushCallback(NoopFlush);
	conn.Register(no_bind_data);
	REQUIRE_THROWS_MATCHES(
	    conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_no_bind_data")).Drain(), Exception,
	    HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto no_init_data = CopyFunction::Create(conn.GetFactory());
	no_init_data.SetName("cpp_copy_no_init_data")
	    .SetCopyToInitCallback(NoopInit)
	    .SetCopyToBatchCallback(NoInitDataBatch)
	    .SetCopyToFlushCallback(NoopFlush);
	conn.Register(no_init_data);
	REQUIRE_THROWS_MATCHES(
	    conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_no_init_data")).Drain(), Exception,
	    HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto no_batch_data = CopyFunction::Create(conn.GetFactory());
	no_batch_data.SetName("cpp_copy_no_batch_data")
	    .SetCopyToBatchCallback(NoopBatch)
	    .SetCopyToFlushCallback(NoBatchDataFlush);
	conn.Register(no_batch_data);
	REQUIRE_THROWS_MATCHES(
	    conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_no_batch_data")).Drain(), Exception,
	    HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto no_global_state = CopyFunction::Create(conn.GetFactory());
	no_global_state.SetName("cpp_copy_no_global_state")
	    .SetCopyFromBindCallback(NoopFromBind)
	    .SetCopyFromExecCallback(NoGlobalStateExec);
	conn.Register(no_global_state);
	REQUIRE_THROWS_MATCHES(conn.Execute(CopyFromStatement("target", path, "cpp_copy_no_global_state")).Drain(),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: copy SetUserData is consumed by Register", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	const auto path = duckdb::TestCreatePath("cpp_copy_consumed.txt");

	auto function = CopyFunction::Create(conn.GetFactory());
	function.SetName("cpp_copy_consumed")
	    .SetUserData<std::string>("first")
	    .SetCopyToBindCallback(UserDataBind)
	    .SetCopyToBatchCallback(NoopBatch)
	    .SetCopyToFlushCallback(NoopFlush);
	conn.Register(function);

	user_data_seen.Reset();
	conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_consumed")).Drain();
	REQUIRE(user_data_seen.in_bind.load() != nullptr);
	REQUIRE(*user_data_seen.in_bind.load() == "first");

	// Register consumed the user data: the second registration has none.
	function.SetName("cpp_copy_consumed2").SetCopyToBindCallback(NoUserDataBind);
	conn.Register(function);
	REQUIRE_THROWS_MATCHES(conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_consumed2")),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST_CASE("Stable C++API: copy function callback errors fail the query", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE target(i BIGINT)").Drain();
	const auto path = duckdb::TestCreatePath("cpp_copy_fails.txt");

	auto function = CopyFunction::Create(conn.GetFactory());
	function.SetName("cpp_copy_fails").SetCopyToBatchCallback(FailingBatch).SetCopyToFlushCallback(NoopFlush);
	conn.Register(function);
	REQUIRE_THROWS_MATCHES(conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_fails")).Drain(),
	                       InvalidInputException, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto reader_fails = CopyFunction::Create(conn.GetFactory());
	reader_fails.SetName("cpp_reader_fails")
	    .SetCopyFromBindCallback(NoopFromBind)
	    .SetCopyFromExecCallback(FailingFromExec);
	conn.Register(reader_fails);
	REQUIRE_THROWS_MATCHES(conn.Execute(CopyFromStatement("target", path, "cpp_reader_fails")).Drain(),
	                       InvalidInputException, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto empty_batch_size = CopyFunction::Create(conn.GetFactory());
	empty_batch_size.SetName("cpp_copy_zero_batch_size")
	    .SetCopyToBatchSizeCallback(ZeroBatchSize)
	    .SetCopyToBatchCallback(NoopBatch)
	    .SetCopyToFlushCallback(NoopFlush);
	conn.Register(empty_batch_size);
	REQUIRE_THROWS_MATCHES(
	    conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_zero_batch_size")),
	    InvalidInputException, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	auto take_twice = CopyFunction::Create(conn.GetFactory());
	take_twice.SetName("cpp_copy_take_twice").SetCopyToBatchCallback(TakeTwiceBatch).SetCopyToFlushCallback(NoopFlush);
	conn.Register(take_twice);
	REQUIRE_THROWS_MATCHES(
	    conn.Execute(CopyToStatement("SELECT r FROM range(3) t(r)", path, "cpp_copy_take_twice")).Drain(),
	    InvalidInputException, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: copy function registration validation", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// No name.
	{
		auto function = CopyFunction::Create(conn.GetFactory());
		function.SetCopyToBatchCallback(NoopBatch).SetCopyToFlushCallback(NoopFlush);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// Neither side configured.
	{
		auto function = CopyFunction::Create(conn.GetFactory());
		function.SetName("cpp_copy_no_side");
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// The COPY TO batch callback missing, then the flush callback.
	{
		auto function = CopyFunction::Create(conn.GetFactory());
		function.SetName("cpp_copy_no_batch").SetCopyToFlushCallback(NoopFlush);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	{
		auto function = CopyFunction::Create(conn.GetFactory());
		function.SetName("cpp_copy_no_flush").SetCopyToBatchCallback(NoopBatch);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// The COPY FROM bind callback missing, then the exec callback.
	{
		auto function = CopyFunction::Create(conn.GetFactory());
		function.SetName("cpp_reader_no_bind").SetCopyFromExecCallback(NoopFromExec);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	{
		auto function = CopyFunction::Create(conn.GetFactory());
		function.SetName("cpp_reader_no_exec").SetCopyFromBindCallback(NoopFromBind);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}
