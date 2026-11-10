#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/compression/dict_fsst/analyze.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace duckdb::dict_fsst;

namespace {

//! Size estimate of the DICT_FSST analyze for 'count' distinct values of 'length' bytes each
idx_t EstimateDistinctValues(BlockManager &block_manager, idx_t count, idx_t length) {
	Vector input(LogicalType::VARCHAR, count);
	{
		auto writer = FlatVector::Writer<string_t>(input, count);
		for (idx_t i = 0; i < count; i++) {
			// the index makes the values distinct, the filler gives them their length
			string value = to_string(i);
			value.resize(length, 'x');
			writer.WriteValue(string_t(value));
		}
	}
	DictFSSTAnalyzeState state(block_manager);
	REQUIRE(state.Analyze(input));
	// values this large cannot be FSST encoded, so the estimate is that of the plain dictionary layout
	REQUIRE(state.disable_fsst);
	return state.FinalAnalyze();
}

//! Size estimate of the DICT_FSST analyze for the values a query returns. A checkpoint analyzes every row group on
//! its own, so the analysis starts over after each of them.
idx_t EstimateQuery(Connection &con, BlockManager &block_manager, const string &query) {
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	auto state = make_uniq<DictFSSTAnalyzeState>(block_manager);
	idx_t estimate = 0;
	idx_t row_group_rows = 0;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		REQUIRE(state->Analyze(chunk->data[0]));
		row_group_rows += chunk->size();
		if (row_group_rows >= DEFAULT_ROW_GROUP_SIZE) {
			estimate += state->FinalAnalyze();
			state = make_uniq<DictFSSTAnalyzeState>(block_manager);
			row_group_rows = 0;
		}
	}
	if (row_group_rows) {
		estimate += state->FinalAnalyze();
	}
	return estimate;
}

//! Bytes the writer needs for the values of a query when DICT_FSST is forced
idx_t WrittenSize(Connection &con, const string &database, const string &query) {
	REQUIRE_NO_FAIL(con.Query("SET force_compression='dict_fsst'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE " + database + ".written AS " + query));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT " + database));
	auto result = con.Query("SELECT used_blocks, block_size FROM pragma_database_size() WHERE database_name = '" +
	                        database + "'");
	REQUIRE_NO_FAIL(*result);
	auto used_blocks = result->Collection().GetValue(0, 0).GetValue<int64_t>();
	auto block_size = result->Collection().GetValue(1, 0).GetValue<int64_t>();
	// one block holds the catalog
	return UnsafeNumericCast<idx_t>((used_blocks - 1) * block_size);
}

} // namespace

TEST_CASE("DICT_FSST size estimate charges segments the way the partial block manager stores them", "[storage]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto path = TestCreatePath("dict_fsst_size_estimate.db");
	DeleteDatabase(path);
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS small (BLOCK_SIZE 16384)"));

	con.BeginTransaction();
	auto &block_manager = StorageManager::Get(Catalog::GetCatalog(*con.context, "small")).GetBlockManager();
	con.Commit();
	// the usable block size is the allocation size minus the block header
	const idx_t block_size = block_manager.GetBlockSize();
	REQUIRE(block_size == 16384 - Storage::DEFAULT_BLOCK_HEADER_SIZE);
	const idx_t partial_block_limit = block_size / 5 * 4;
	const idx_t count = 40;

	// Only one of these values fits in a block, so every value becomes a segment of its own. A segment that takes
	// up to 80% of a block is packed with other segments, so it is charged its own size.
	const idx_t shared_length = 9000;
	const idx_t shared_segment =
	    AlignValue<idx_t>(DictFSSTAnalyzeState::RequiredSpace(1, 1, shared_length, shared_length));
	REQUIRE(shared_segment <= partial_block_limit);
	REQUIRE(EstimateDistinctValues(block_manager, count, shared_length) == count * shared_segment);

	// A segment above that limit gets a block of its own and the rest of the block is lost.
	const idx_t exclusive_length = 14000;
	const idx_t exclusive_segment = DictFSSTAnalyzeState::RequiredSpace(1, 1, exclusive_length, exclusive_length);
	REQUIRE(exclusive_segment > partial_block_limit);
	REQUIRE(exclusive_segment < block_size);
	REQUIRE(EstimateDistinctValues(block_manager, count, exclusive_length) == count * block_size);
}

TEST_CASE("DICT_FSST size estimate follows the way the writer encodes a segment", "[storage]") {
	struct Scenario {
		string name;
		string values;
		idx_t rows;
		idx_t block_alloc_size;
	};
	const string hex_text = "string_agg(md5((hash(i) % 80)::VARCHAR || '-' || x::VARCHAR), '')";
	vector<Scenario> scenarios = {
	    // the dictionary is FSST encoded and fits a single block, which is what the estimate has to predict
	    {"encoded dictionary", "(SELECT " + hex_text + " FROM range(180) x)", 2000, 262144},
	    // FSST_ONLY for every segment but the first, which has to store the repeated value in a dictionary
	    {"unique values with a duplicate", "CASE WHEN i = 1 THEN '0' ELSE i::VARCHAR END || repeat('abcdefgh', 500)",
	     20000, 262144},
	    // NULLs rule out FSST_ONLY, so the selection buffer is paid for in every segment
	    {"unique values with NULLs", "CASE WHEN i % 10 = 0 THEN NULL ELSE i::VARCHAR || repeat('abcdefgh', 500) END",
	     20000, 262144},
	    {"small blocks", "i::VARCHAR || repeat('abcdefgh', 120)", 20000, 16384},
	    // a dictionary that does not compress is left as it is
	    {"incompressible values",
	     "(SELECT string_agg(chr(33 + (hash(i * 3000 + x) % 90)::INT), '') FROM range(2000) x)", 4000, 262144},
	};
	for (auto &scenario : scenarios) {
		DuckDB db(nullptr);
		Connection con(db);
		auto path = TestCreatePath("dict_fsst_size_estimate_writer.db");
		DeleteDatabase(path);
		REQUIRE_NO_FAIL(con.Query(
		    StringUtil::Format("ATTACH '%s' AS written_db (BLOCK_SIZE %d)", path, scenario.block_alloc_size)));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE TABLE source AS SELECT %s AS v FROM range(%d) t(i)",
		                                             scenario.values, scenario.rows)));

		con.BeginTransaction();
		auto &block_manager = StorageManager::Get(Catalog::GetCatalog(*con.context, "written_db")).GetBlockManager();
		con.Commit();

		const auto estimate = EstimateQuery(con, block_manager, "SELECT v FROM source");
		const auto written = WrittenSize(con, "written_db", "SELECT v FROM source");
		INFO(scenario.name << ": estimate " << estimate << ", written " << written);
		// the estimate does not train a symbol table per segment, so it is not exact
		CHECK(estimate * 100 >= written * 85);
		CHECK(estimate * 100 <= written * 115);
	}
}
