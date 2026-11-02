#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

// Appends an empty transient segment that starts after a gap in the row ids, past the end of the column
idx_t AppendSegmentAfterGap(ClientContext &context, ColumnData &column) {
	auto &segment_tree = column.GetSegmentTree();
	auto lock = segment_tree.Lock();
	auto end = segment_tree.GetLastSegment(lock)->GetRowEnd();
	auto &db = DatabaseInstance::GetDatabase(context);
	auto function = DBConfig::GetConfig(db).GetCompressionFunction(CompressionType::COMPRESSION_UNCOMPRESSED,
	                                                               column.type.InternalType());
	auto segment = ColumnSegment::CreateTransientSegment(
	    db, function.get(), column.type, column.GetBlockManager().GetBlockSize(), column.GetBlockManager());
	segment_tree.AppendSegment(lock, shared_ptr<ColumnSegment>(std::move(segment)), end + STANDARD_VECTOR_SIZE);
	return end;
}

} // namespace

TEST_CASE("Test that reverting an append to a row in a gap of the column segment tree reports corruption",
          "[storage]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT range FROM range(100)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	auto &table =
	    Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(INVALID_CATALOG, DEFAULT_SCHEMA, "t"))
	        .Cast<DuckTableEntry>();
	auto &column = table.GetStorage().GetRowGroupCollection()->GetRowGroup(0)->GetRawColumnData(0);
	auto end = AppendSegmentAfterGap(*con.context, column);
	REQUIRE(end == 100);

	// the row lies in the gap before the appended segment
	REQUIRE_THROWS_AS(column.RevertAppend(UnsafeNumericCast<row_t>(end)), DataCorruptionException);
	{
		auto &segment_tree = column.GetSegmentTree();
		auto lock = segment_tree.Lock();
		REQUIRE(segment_tree.GetSegmentCount(lock) == 2);
		idx_t segment_index;
		REQUIRE(!segment_tree.TryGetSegmentIndex(lock, end, segment_index));
		REQUIRE_THROWS_AS(segment_tree.GetSegmentIndex(lock, end), DataCorruptionException);
	}
	REQUIRE(column.count == 100);
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
}
