#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_access_intent.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

struct RecordedLookup {
	string name;
	CatalogAccessIntent intent;
};

//! The lookups a LookupRecordingCatalog saw, shared through the storage extension
struct LookupLog : StorageExtensionInfo {
	mutex lock;
	vector<RecordedLookup> lookups;

	void Clear() {
		lock_guard<mutex> guard(lock);
		lookups.clear();
	}
	//! The intents of every lookup of 'name' since the last Clear
	vector<CatalogAccessIntent> IntentsOf(const string &name) {
		lock_guard<mutex> guard(lock);
		vector<CatalogAccessIntent> result;
		for (auto &lookup : lookups) {
			if (lookup.name == name) {
				result.push_back(lookup.intent);
			}
		}
		return result;
	}
};

//! A DuckDB catalog that records the access intent of every entry lookup (schemas excluded)
class LookupRecordingCatalog : public DuckCatalog {
public:
	LookupRecordingCatalog(AttachedDatabase &db, LookupLog &log) : DuckCatalog(db), log(log) {
	}

private:
	CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction,
	                                          const EntryLookupInfo &lookup_info) override {
		{
			lock_guard<mutex> guard(log.lock);
			log.lookups.push_back({lookup_info.GetEntryName(), lookup_info.GetAccessIntent()});
		}
		// what Catalog::TryLookupEntryInternal does (the tests use no AT clause)
		auto &full_path = lookup_info.GetQualifiedName().Path();
		vector<Identifier> schema_path(full_path.begin(), full_path.end() - 1);
		auto schema_lookup = EntryLookupInfo::SchemaLookup(lookup_info, std::move(schema_path));
		auto schema = LookupSchema(transaction, schema_lookup, OnEntryNotFound::RETURN_NULL);
		if (!schema) {
			return {nullptr, nullptr, ErrorData()};
		}
		return {schema, schema->LookupEntry(transaction, lookup_info), ErrorData()};
	}

	LookupLog &log;
};

struct LookupRecordingStorageExtension : StorageExtension {
	explicit LookupRecordingStorageExtension(shared_ptr<LookupLog> log) {
		storage_info = std::move(log);
		attach = [](optional_ptr<StorageExtensionInfo> storage_info, ClientContext &, AttachedDatabase &db,
		            const string &, AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq<LookupRecordingCatalog>(db, static_cast<LookupLog &>(*storage_info));
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

string IntentsToString(const vector<CatalogAccessIntent> &intents) {
	vector<string> result;
	for (auto &intent : intents) {
		result.push_back(intent.ToString());
	}
	return "[" + StringUtil::Join(result, ", ") + "]";
}

//! Every lookup of 'name' has the intent 'expected' (and there is at least 'min_lookups' of them)
void RequireIntents(LookupLog &log, const string &name, CatalogAccessIntent expected, idx_t min_lookups = 1) {
	auto intents = log.IntentsOf(name);
	INFO(name << " looked up with " << IntentsToString(intents) << ", expected " << expected.ToString());
	REQUIRE(intents.size() >= min_lookups);
	for (auto &intent : intents) {
		REQUIRE(intent == expected);
	}
}

void RequireRun(Connection &con, LookupLog &log, const string &sql) {
	log.Clear();
	auto result = con.Query(sql);
	INFO(sql);
	REQUIRE_NO_FAIL(*result);
}

} // namespace

TEST_CASE("CatalogAccessIntent predicates and ToString", "[catalog]") {
	CatalogAccessIntent read;
	REQUIRE(read == CatalogAccessIntent::DEFAULT);
	REQUIRE(read.ReadsData());
	REQUIRE(!read.IsWrite());
	REQUIRE(!read.ReadsMetadataOnly());
	REQUIRE(read.ToString() == "READ_METADATA | READ_DATA");

	CatalogAccessIntent metadata(CatalogAccessIntent::READ_METADATA);
	REQUIRE(metadata.ReadsMetadataOnly());
	REQUIRE(!metadata.ReadsData());
	REQUIRE(!metadata.IsWrite());

	CatalogAccessIntent update = CatalogAccessIntent::UPDATE_DATA | CatalogAccessIntent::READ_DATA;
	REQUIRE(update.ModifiesData());
	REQUIRE(!update.ModifiesMetadata());
	REQUIRE(update.IsWrite());
	REQUIRE(update.ReadsData());
	REQUIRE(update.Contains(CatalogAccessIntent::UPDATE_DATA));
	REQUIRE(!update.Contains(CatalogAccessIntent::UPDATE_DATA | CatalogAccessIntent::DELETE_DATA));
	REQUIRE(update.ToString() == "READ_DATA | UPDATE_DATA");

	CatalogAccessIntent merge = CatalogAccessIntent::READ_DATA;
	merge |= CatalogAccessIntent::INSERT_DATA;
	merge |= CatalogAccessIntent::DELETE_DATA;
	REQUIRE(merge ==
	        (CatalogAccessIntent::READ_DATA | CatalogAccessIntent::INSERT_DATA | CatalogAccessIntent::DELETE_DATA));

	for (auto intent : {CatalogAccessIntent::MODIFY_METADATA, CatalogAccessIntent::DROP_ENTRY}) {
		CatalogAccessIntent ddl(intent);
		REQUIRE(ddl.ModifiesMetadata());
		REQUIRE(!ddl.ModifiesData());
		REQUIRE(ddl.IsWrite());
		REQUIRE(!ddl.ReadsData());
	}
	REQUIRE(CatalogAccessIntent(CatalogAccessIntent::DROP_ENTRY).ToString() == "DROP_ENTRY");
	REQUIRE(CatalogAccessIntent(0).ToString() == "NONE");
}

TEST_CASE("EntryLookupInfo carries its access intent to derived lookups", "[catalog]") {
	EntryLookupInfo read(CatalogType::TABLE_ENTRY, QualifiedName(Identifier("t")));
	REQUIRE(read.GetAccessIntent() == CatalogAccessIntent::DEFAULT);

	EntryLookupInfo insert(CatalogType::TABLE_ENTRY, QualifiedName(Identifier("t")), nullptr, QueryErrorContext(),
	                       CatalogAccessIntent::INSERT_DATA);
	REQUIRE(insert.GetAccessIntent() == CatalogAccessIntent::INSERT_DATA);
	REQUIRE(
	    EntryLookupInfo(insert, QualifiedName(Identifier("c"), Identifier("s"), Identifier("t"))).GetAccessIntent() ==
	    CatalogAccessIntent::INSERT_DATA);
	REQUIRE(EntryLookupInfo(insert, optional_ptr<BoundAtClause>()).GetAccessIntent() ==
	        CatalogAccessIntent::INSERT_DATA);
	REQUIRE(EntryLookupInfo(insert, CatalogAccessIntent::DROP_ENTRY).GetAccessIntent() ==
	        CatalogAccessIntent::DROP_ENTRY);
	// a schema lookup reads the schema; the intent is for the entry
	REQUIRE(EntryLookupInfo::SchemaLookup(insert, {Identifier("s")}).GetAccessIntent() == CatalogAccessIntent::DEFAULT);
}

TEST_CASE("Statements look their target up with its access intent and their sources with DEFAULT", "[catalog]") {
	auto log = make_shared_ptr<LookupLog>();
	DBConfig config;
	StorageExtension::Register(config, "lookup_recorder", make_shared_ptr<LookupRecordingStorageExtension>(log));
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS rec (TYPE lookup_recorder)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE rec.target (i INTEGER, j VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE rec.source AS SELECT i, 's' || i AS j FROM range(1, 4) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE rec.keyed (i INTEGER PRIMARY KEY, j VARCHAR)"));

	const CatalogAccessIntent read = CatalogAccessIntent::DEFAULT;
	const CatalogAccessIntent update = CatalogAccessIntent::UPDATE_DATA | CatalogAccessIntent::READ_DATA;
	const CatalogAccessIntent remove = CatalogAccessIntent::DELETE_DATA | CatalogAccessIntent::READ_DATA;

	SECTION("SELECT") {
		RequireRun(con, *log, "SELECT * FROM rec.target");
		RequireIntents(*log, "target", read);
	}
	SECTION("INSERT") {
		RequireRun(con, *log, "INSERT INTO rec.target VALUES (1, 'a')");
		RequireIntents(*log, "target", CatalogAccessIntent::INSERT_DATA);
		RequireRun(con, *log, "INSERT INTO rec.target SELECT * FROM rec.source");
		RequireIntents(*log, "target", CatalogAccessIntent::INSERT_DATA);
		RequireIntents(*log, "source", read);
	}
	SECTION("INSERT reading its own target") {
		RequireRun(con, *log, "INSERT INTO rec.target SELECT * FROM rec.target");
		auto intents = log->IntentsOf("target");
		INFO(IntentsToString(intents));
		REQUIRE(std::count(intents.begin(), intents.end(), CatalogAccessIntent::INSERT_DATA) >= 1);
		REQUIRE(std::count(intents.begin(), intents.end(), read) >= 1);
	}
	SECTION("INSERT ... ON CONFLICT DO UPDATE") {
		// binds as an INSERT, then as a MERGE INTO with an insert and an update action
		RequireRun(con, *log, "INSERT INTO rec.keyed VALUES (1, 'x') ON CONFLICT DO UPDATE SET j = excluded.j");
		auto intents = log->IntentsOf("keyed");
		INFO(IntentsToString(intents));
		REQUIRE(!intents.empty());
		REQUIRE(intents.front() == CatalogAccessIntent::INSERT_DATA);
		REQUIRE(std::count(intents.begin(), intents.end(), update | CatalogAccessIntent::INSERT_DATA) >= 1);
		for (auto &intent : intents) {
			REQUIRE(intent.Contains(CatalogAccessIntent::INSERT_DATA));
		}
	}
	SECTION("COPY FROM") {
		auto path = TestCreatePath("catalog_access_intent.csv");
		REQUIRE_NO_FAIL(con.Query("COPY (SELECT 7 AS i, 'c' AS j) TO '" + path + "'"));
		RequireRun(con, *log, "COPY rec.target FROM '" + path + "'");
		RequireIntents(*log, "target", CatalogAccessIntent::INSERT_DATA);
	}
	SECTION("UPDATE") {
		RequireRun(con, *log, "UPDATE rec.target SET j = 'u' WHERE i = 1");
		RequireIntents(*log, "target", update);
		RequireRun(con, *log, "UPDATE rec.target SET j = source.j FROM rec.source WHERE target.i = source.i");
		RequireIntents(*log, "target", update);
		RequireIntents(*log, "source", read);
	}
	SECTION("DELETE and TRUNCATE") {
		RequireRun(con, *log, "DELETE FROM rec.target USING rec.source WHERE target.i = source.i");
		RequireIntents(*log, "target", remove);
		RequireIntents(*log, "source", read);
		RequireRun(con, *log, "TRUNCATE rec.target");
		RequireIntents(*log, "target", remove);
	}
	SECTION("MERGE INTO carries the union of its actions") {
		RequireRun(
		    con, *log,
		    "MERGE INTO rec.target USING rec.source ON target.i = source.i "
		    "WHEN MATCHED THEN UPDATE SET j = source.j WHEN NOT MATCHED THEN INSERT VALUES (source.i, source.j)");
		RequireIntents(*log, "target", update | CatalogAccessIntent::INSERT_DATA);
		RequireIntents(*log, "source", read);
		RequireRun(con, *log, "MERGE INTO rec.target USING rec.source ON target.i = source.i WHEN MATCHED THEN DELETE");
		RequireIntents(*log, "target", remove);
		RequireRun(con, *log,
		           "MERGE INTO rec.target USING rec.source ON target.i = source.i "
		           "WHEN NOT MATCHED THEN INSERT VALUES (source.i, source.j)");
		RequireIntents(*log, "target", CatalogAccessIntent::INSERT_DATA | CatalogAccessIntent::READ_DATA);
	}
	SECTION("ALTER and COMMENT ON, including the lookup Catalog::Alter makes when it executes") {
		RequireRun(con, *log, "ALTER TABLE rec.target ADD COLUMN k INTEGER");
		RequireIntents(*log, "target", CatalogAccessIntent::MODIFY_METADATA, 2);
		RequireRun(con, *log, "ALTER TABLE rec.target RENAME COLUMN k TO l");
		RequireIntents(*log, "target", CatalogAccessIntent::MODIFY_METADATA, 2);
		RequireRun(con, *log, "COMMENT ON TABLE rec.target IS 'table comment'");
		RequireIntents(*log, "target", CatalogAccessIntent::MODIFY_METADATA, 2);
		RequireRun(con, *log, "COMMENT ON COLUMN rec.target.i IS 'column comment'");
		RequireIntents(*log, "target", CatalogAccessIntent::MODIFY_METADATA, 2);
	}
	SECTION("DROP, including the lookup Catalog::DropEntry makes when it executes") {
		REQUIRE_NO_FAIL(con.Query("CREATE VIEW rec.source_view AS SELECT * FROM rec.source"));
		RequireRun(con, *log, "DROP VIEW rec.source_view");
		RequireIntents(*log, "source_view", CatalogAccessIntent::DROP_ENTRY, 2);
		RequireRun(con, *log, "DROP TABLE rec.target");
		RequireIntents(*log, "target", CatalogAccessIntent::DROP_ENTRY, 2);
	}
	SECTION("EXPLAIN binds the same lookups") {
		RequireRun(con, *log, "EXPLAIN UPDATE rec.target SET j = source.j FROM rec.source WHERE target.i = source.i");
		RequireIntents(*log, "target", update);
		RequireIntents(*log, "source", read);
	}
	SECTION("CREATE TABLE AS has no target lookup") {
		RequireRun(con, *log, "CREATE TABLE rec.created AS SELECT * FROM rec.source");
		RequireIntents(*log, "source", read);
		for (auto &intent : log->IntentsOf("created")) {
			REQUIRE(!intent.IsWrite());
		}
	}
	SECTION("the statements behave as on any other DuckDB catalog") {
		REQUIRE_NO_FAIL(con.Query("INSERT INTO rec.target SELECT * FROM rec.source"));
		REQUIRE_NO_FAIL(con.Query("UPDATE rec.target SET j = 'u' WHERE i = 1"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM rec.target WHERE i = 2"));
		REQUIRE_NO_FAIL(con.Query("MERGE INTO rec.target USING rec.source ON target.i = source.i "
		                          "WHEN NOT MATCHED THEN INSERT VALUES (source.i, 'm')"));
		auto result = con.Query("SELECT i, j FROM rec.target ORDER BY i");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));
		REQUIRE(CHECK_COLUMN(result, 1, {"u", "m", "s3"}));
	}
}
