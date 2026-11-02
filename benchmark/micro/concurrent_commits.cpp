#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"

#include <chrono>
#include <thread>
#include <vector>

using namespace duckdb;

// A LocalFileSystem whose fsync is a fixed delay, emulating high-latency durable storage (e.g.
// networked disks). A zero delay skips the fsync altogether, which measures the commit path itself
class DelayFsyncFileSystem : public LocalFileSystem {
public:
	explicit DelayFsyncFileSystem(int64_t delay_us_p) : delay_us(delay_us_p) {
	}
	void FileSync(FileHandle &handle) override {
		if (delay_us > 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
		}
	}

private:
	int64_t delay_us;
};

// Owns a persistent database whose WAL fsync latency is simulated. The base
// DuckDBBenchmarkState db/conn (in-memory) are unused; all work runs on persistent_db.
struct ConcurrentCommitState : public DuckDBBenchmarkState {
	duckdb::unique_ptr<DuckDB> persistent_db;

	explicit ConcurrentCommitState(int64_t delay_us) : DuckDBBenchmarkState(string()) {
		DBConfig config;
		config.file_system = make_uniq<VirtualFileSystem>(make_uniq<DelayFsyncFileSystem>(delay_us));
		string path = "duckdb_concurrent_commits_bench.db";
		DeleteDatabase(path);
		persistent_db = make_uniq<DuckDB>(path, &config);
		Connection con(*persistent_db);
		// keep the whole run on the WAL path - do not checkpoint mid-run
		con.Query("SET checkpoint_threshold='1TB'");
	}
};

// NUM_THREADS connections each commit COMMITS_PER_THREAD single-row INSERTs. Every auto-commit
// INSERT is its own transaction, so concurrent committers share their WAL fsyncs
struct ConcurrentCommits {
	static void Load(DuckDBBenchmarkState *state_p) {
		auto state = (ConcurrentCommitState *)state_p;
		Connection con(*state->persistent_db);
		con.Query("CREATE TABLE integers(i INTEGER, t INTEGER)");
	}
	static void Run(DuckDBBenchmarkState *state_p, int64_t num_threads, int64_t commits_per_thread) {
		auto state = (ConcurrentCommitState *)state_p;
		std::vector<std::thread> threads;
		for (int64_t t = 0; t < num_threads; t++) {
			threads.emplace_back([state, t, commits_per_thread]() {
				Connection con(*state->persistent_db);
				for (int64_t i = 0; i < commits_per_thread; i++) {
					con.Query("INSERT INTO integers VALUES (" + std::to_string(i) + ", " + std::to_string(t) + ")");
				}
			});
		}
		for (auto &thread : threads) {
			thread.join();
		}
	}
	static void Cleanup(DuckDBBenchmarkState *state_p) {
		auto state = (ConcurrentCommitState *)state_p;
		Connection con(*state->persistent_db);
		con.Query("DROP TABLE integers");
		con.Query("CREATE TABLE integers(i INTEGER, t INTEGER)");
	}
	static string Info(int64_t num_threads, int64_t commits_per_thread, int64_t delay_us) {
		return std::to_string(num_threads) + " threads commit " + std::to_string(num_threads * commits_per_thread) +
		       " INSERT transactions, " + std::to_string(delay_us) + "us simulated WAL fsync latency";
	}
};

#define CONCURRENT_COMMITS_BENCHMARK(NUM_THREADS, COMMITS_PER_THREAD, DELAY_US)                                        \
	duckdb::unique_ptr<DuckDBBenchmarkState> CreateBenchmarkState() override {                                         \
		return make_uniq<ConcurrentCommitState>(DELAY_US);                                                             \
	}                                                                                                                  \
	void Load(DuckDBBenchmarkState *state) override {                                                                  \
		ConcurrentCommits::Load(state);                                                                                \
	}                                                                                                                  \
	void RunBenchmark(DuckDBBenchmarkState *state) override {                                                          \
		ConcurrentCommits::Run(state, NUM_THREADS, COMMITS_PER_THREAD);                                                \
	}                                                                                                                  \
	void Cleanup(DuckDBBenchmarkState *state) override {                                                               \
		ConcurrentCommits::Cleanup(state);                                                                             \
	}                                                                                                                  \
	string VerifyResult(QueryResult *result) override {                                                                \
		return string();                                                                                               \
	}                                                                                                                  \
	string BenchmarkInfo() override {                                                                                  \
		return ConcurrentCommits::Info(NUM_THREADS, COMMITS_PER_THREAD, DELAY_US);                                     \
	}

// no fsync (fixed 16000 total commits)
DUCKDB_BENCHMARK(ConcurrentCommits1Thread, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(1, 16000, 0)
FINISH_BENCHMARK(ConcurrentCommits1Thread)

DUCKDB_BENCHMARK(ConcurrentCommits4Threads, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(4, 4000, 0)
FINISH_BENCHMARK(ConcurrentCommits4Threads)

DUCKDB_BENCHMARK(ConcurrentCommits8Threads, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(8, 2000, 0)
FINISH_BENCHMARK(ConcurrentCommits8Threads)

DUCKDB_BENCHMARK(ConcurrentCommits16Threads, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(16, 1000, 0)
FINISH_BENCHMARK(ConcurrentCommits16Threads)

DUCKDB_BENCHMARK(ConcurrentCommits32Threads, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(32, 500, 0)
FINISH_BENCHMARK(ConcurrentCommits32Threads)

// 1ms simulated fsync latency (fixed 800 total commits)
DUCKDB_BENCHMARK(ConcurrentCommits1Thread1ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(1, 800, 1000)
FINISH_BENCHMARK(ConcurrentCommits1Thread1ms)

DUCKDB_BENCHMARK(ConcurrentCommits4Threads1ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(4, 200, 1000)
FINISH_BENCHMARK(ConcurrentCommits4Threads1ms)

DUCKDB_BENCHMARK(ConcurrentCommits8Threads1ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(8, 100, 1000)
FINISH_BENCHMARK(ConcurrentCommits8Threads1ms)

DUCKDB_BENCHMARK(ConcurrentCommits16Threads1ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(16, 50, 1000)
FINISH_BENCHMARK(ConcurrentCommits16Threads1ms)

DUCKDB_BENCHMARK(ConcurrentCommits32Threads1ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(32, 25, 1000)
FINISH_BENCHMARK(ConcurrentCommits32Threads1ms)

// 10ms simulated fsync latency (fixed 160 total commits)
DUCKDB_BENCHMARK(ConcurrentCommits1Thread10ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(1, 160, 10000)
FINISH_BENCHMARK(ConcurrentCommits1Thread10ms)

DUCKDB_BENCHMARK(ConcurrentCommits4Threads10ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(4, 40, 10000)
FINISH_BENCHMARK(ConcurrentCommits4Threads10ms)

DUCKDB_BENCHMARK(ConcurrentCommits8Threads10ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(8, 20, 10000)
FINISH_BENCHMARK(ConcurrentCommits8Threads10ms)

DUCKDB_BENCHMARK(ConcurrentCommits16Threads10ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(16, 10, 10000)
FINISH_BENCHMARK(ConcurrentCommits16Threads10ms)

DUCKDB_BENCHMARK(ConcurrentCommits32Threads10ms, "[wal]")
CONCURRENT_COMMITS_BENCHMARK(32, 5, 10000)
FINISH_BENCHMARK(ConcurrentCommits32Threads10ms)
