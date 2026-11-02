#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: ArrowResult. Executing with an ArrowFormat, reading
// arrays, the consuming stream conversion, and the lifetimes the wrapper
// promises. The C-level semantics are pinned in test_capi_v2_arrow.cpp.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

struct OwnedSchema {
	ArrowSchema schema {};
	~OwnedSchema() {
		if (schema.release) {
			schema.release(&schema);
		}
	}
};

struct OwnedStream {
	ArrowArrayStream stream {};
	~OwnedStream() {
		if (stream.release) {
			stream.release(&stream);
		}
	}
};

// Arrays collected from a result, released together.
struct ArrowBatches {
	std::vector<ArrowArray> arrays;
	ArrowBatches() = default;
	ArrowBatches(const ArrowBatches &) = delete;
	ArrowBatches &operator=(const ArrowBatches &) = delete;
	~ArrowBatches() {
		for (auto &array : arrays) {
			if (array.release) {
				array.release(&array);
			}
		}
	}
	idx_t RowCount() const {
		idx_t rows = 0;
		for (auto &array : arrays) {
			rows += static_cast<idx_t>(array.length);
		}
		return rows;
	}
	idx_t MaxLength() const {
		idx_t longest = 0;
		for (auto &array : arrays) {
			longest = std::max<idx_t>(longest, static_cast<idx_t>(array.length));
		}
		return longest;
	}
};

void FetchAll(ArrowResult &result, ArrowBatches &out) {
	while (true) {
		ArrowArray array {};
		if (!result.FetchArray(array)) {
			break;
		}
		out.arrays.push_back(array);
	}
}

// Drains a C stream; asserted once by the caller so the assertion count does not depend on the array count.
int StreamAll(ArrowArrayStream &stream, ArrowBatches &out) {
	while (true) {
		ArrowArray array {};
		auto rc = stream.get_next(&stream, &array);
		if (rc != 0 || !array.release) {
			return rc;
		}
		out.arrays.push_back(array);
	}
}

idx_t RowIndex(const ArrowArray &batch, idx_t column, idx_t row) {
	return static_cast<idx_t>(batch.offset + batch.children[column]->offset) + row;
}

int64_t Int64At(const ArrowArray &batch, idx_t column, idx_t row) {
	return static_cast<const int64_t *>(batch.children[column]->buffers[1])[RowIndex(batch, column, row)];
}

// An array in the "u" format: 32-bit offsets.
std::string StringAt(const ArrowArray &batch, idx_t column, idx_t row) {
	auto &strings = *batch.children[column];
	auto offsets = static_cast<const int32_t *>(strings.buffers[1]);
	auto data = static_cast<const char *>(strings.buffers[2]);
	auto index = RowIndex(batch, column, row);
	return std::string(data + offsets[index], static_cast<size_t>(offsets[index + 1] - offsets[index]));
}

// Whether column 0, a BIGINT, counts up from `first` across all arrays.
bool CountsUpFrom(const ArrowBatches &batches, int64_t first) {
	auto expected = first;
	for (auto &array : batches.arrays) {
		for (idx_t row = 0; row < static_cast<idx_t>(array.length); row++) {
			if (Int64At(array, 0, row) != expected++) {
				return false;
			}
		}
	}
	return true;
}

std::vector<std::string> ChildNames(const ArrowSchema &schema) {
	std::vector<std::string> names;
	for (int64_t i = 0; i < schema.n_children; i++) {
		names.emplace_back(schema.children[i]->name);
	}
	return names;
}

std::vector<std::string> ChildFormats(const ArrowSchema &schema) {
	std::vector<std::string> formats;
	for (int64_t i = 0; i < schema.n_children; i++) {
		formats.emplace_back(schema.children[i]->format);
	}
	return formats;
}

SqlStatement ParseOne(Connection &conn, const char *sql) {
	auto statements = conn.ParseSQL(sql);
	return statements.Next();
}

// Sentinels the library must clear: the wrapper resets `release` before every call.
void SentinelRelease(ArrowArray *array) {
	array->release = nullptr;
}
void SentinelSchemaRelease(ArrowSchema *schema) {
	schema->release = nullptr;
}
void SentinelStreamRelease(ArrowArrayStream *stream) {
	stream->release = nullptr;
}

} // namespace

TEST_CASE("Stable C++API: ArrowResult fetch loop delivers batched arrays in order", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto result = conn.Execute("SELECT i FROM range(10000) t(i)", ArrowFormat {1000});

	ArrowBatches batches;
	FetchAll(result, batches);
	REQUIRE(batches.RowCount() == 10000);
	REQUIRE(batches.MaxLength() <= 1000);
	REQUIRE(batches.arrays.size() >= 10);
	REQUIRE(CountsUpFrom(batches, 0));

	// The end is sticky, and `out` stays released.
	ArrowArray array {};
	array.release = SentinelRelease;
	REQUIRE_FALSE(result.FetchArray(array));
	REQUIRE(array.release == nullptr);
	REQUIRE_FALSE(result.FetchArray(array));
}

TEST_CASE("Stable C++API: ArrowResult step loop drains a result without blocking", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto statement = ParseOne(conn, "SELECT i FROM range(5000) t(i)");
	auto result = conn.Execute(statement, ArrowFormat {700});

	// Collect flags in the loop and assert once after, so the assertion count
	// does not depend on scheduling.
	ArrowBatches batches;
	bool array_iff_chunk = true;
	while (true) {
		ArrowArray array {};
		auto status = result.Step(array);
		if (status == StepStatus::CHUNK) {
			array_iff_chunk &= array.release != nullptr;
			batches.arrays.push_back(array);
			continue;
		}
		array_iff_chunk &= array.release == nullptr;
		if (status == StepStatus::WAITING) {
			result.Wait();
			continue;
		}
		REQUIRE(status == StepStatus::FINISHED);
		break;
	}
	REQUIRE(array_iff_chunk);
	REQUIRE(batches.RowCount() == 5000);
	REQUIRE(batches.MaxLength() <= 700);
	REQUIRE(CountsUpFrom(batches, 0));

	// FINISHED is sticky, and `out` is reset even when no array comes.
	ArrowArray array {};
	array.release = SentinelRelease;
	REQUIRE(result.Step(array) == StepStatus::FINISHED);
	REQUIRE(array.release == nullptr);
}

TEST_CASE("Stable C++API: ArrowFormat's default batch size is the engine's 131072", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	// One thread gives exact batch sizes.
	conn.Execute("SET threads = 1").Drain();

	auto Lengths = [](ArrowResult result) {
		ArrowBatches batches;
		FetchAll(result, batches);
		std::vector<int64_t> lengths;
		for (auto &array : batches.arrays) {
			lengths.push_back(array.length);
		}
		return lengths;
	};
	auto expected = std::vector<int64_t> {131072, 8928};

	REQUIRE(Lengths(conn.Execute("SELECT i FROM range(140000) t(i)", ArrowFormat {})) == expected);

	auto statement = ParseOne(conn, "SELECT i FROM range(140000) t(i)");
	auto prepared = conn.Prepare(statement);
	REQUIRE(Lengths(prepared.Execute(ArrowFormat {})) == expected);
}

TEST_CASE("Stable C++API: ArrowResult reports its schema and types", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	OwnedSchema survivor;
	{
		auto result = conn.Execute("SELECT 42::BIGINT AS answer, 'x' AS label", ArrowFormat {});

		OwnedSchema before;
		result.GetSchema(before.schema);
		REQUIRE(std::string(before.schema.format) == "+s");
		REQUIRE(ChildNames(before.schema) == std::vector<std::string> {"answer", "label"});
		REQUIRE(ChildFormats(before.schema) == std::vector<std::string> {"l", "u"});

		REQUIRE(result.GetResultType() == ResultType::QUERY_RESULT);
		REQUIRE(result.GetStatementType() == StatementType::SELECT);

		ArrowBatches batches;
		FetchAll(result, batches);
		REQUIRE(batches.RowCount() == 1);
		REQUIRE(Int64At(batches.arrays[0], 0, 0) == 42);
		REQUIRE(StringAt(batches.arrays[0], 1, 0) == "x");

		// Each call is an independent copy; this one outlives the result.
		result.GetSchema(survivor.schema);
	}
	REQUIRE(ChildNames(survivor.schema) == std::vector<std::string> {"answer", "label"});
}

TEST_CASE("Stable C++API: arrays and schemas outlive the result and the database", "[cpp_api][arrow]") {
	OwnedSchema schema;
	ArrowBatches batches;
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		auto result = conn.Execute("SELECT i FROM range(5) t(i)", ArrowFormat {});
		result.GetSchema(schema.schema);
		FetchAll(result, batches);
	}
	// The result, connection, instance and environment are gone; the handed-out
	// structs still read, and their release callbacks still run (RAII, under ASan).
	REQUIRE(ChildNames(schema.schema) == std::vector<std::string> {"i"});
	REQUIRE(batches.RowCount() == 5);
	REQUIRE(CountsUpFrom(batches, 0));
}

TEST_CASE("Stable C++API: ArrowResult of an expanding statement defers its schema", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE sales(product VARCHAR, quarter VARCHAR, amount INTEGER)").Drain();
	conn.Execute("INSERT INTO sales VALUES ('a', 'q1', 1), ('a', 'q2', 2)").Drain();

	auto result = conn.Execute("PIVOT sales ON quarter USING sum(amount)", ArrowFormat {});

	// PIVOT expands into several statements, so the metadata is not known yet,
	// and a failed GetSchema leaves `out` released.
	OwnedSchema early;
	early.schema.release = SentinelSchemaRelease;
	REQUIRE_THROWS_MATCHES(result.GetSchema(early.schema), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED));
	REQUIRE(early.schema.release == nullptr);
	REQUIRE_THROWS_MATCHES(result.GetResultType(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED));
	REQUIRE(result.GetStatementType() == StatementType::MULTI);

	ArrowBatches batches;
	FetchAll(result, batches);
	REQUIRE(batches.RowCount() == 1);

	OwnedSchema late;
	result.GetSchema(late.schema);
	REQUIRE(ChildNames(late.schema) == std::vector<std::string> {"product", "q1", "q2"});
	REQUIRE(result.GetResultType() == ResultType::QUERY_RESULT);
}

TEST_CASE("Stable C++API: ArrowResult binds parameters through every Execute form", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto Check = [](ArrowResult result) {
		ArrowBatches batches;
		FetchAll(result, batches);
		REQUIRE(batches.RowCount() == 10);
		REQUIRE(batches.MaxLength() <= 4);
		REQUIRE(CountsUpFrom(batches, 10));
	};

	auto positional = ParseOne(conn, "SELECT $1::BIGINT + i FROM range(10) t(i)");

	{
		auto value = Value::Create(conn.GetFactory(), int64_t(10));
		Check(conn.Execute(positional, &value, 1, ArrowFormat {4}));
	}
	{
		std::vector<Value> params;
		params.push_back(Value::Create(conn.GetFactory(), int64_t(10)));
		Check(conn.Execute(positional, params, ArrowFormat {4}));
	}
	{
		// An empty name binds positionally.
		std::vector<NamedParam> params;
		params.push_back({"", Value::Create(conn.GetFactory(), int64_t(10))});
		Check(conn.Execute(positional, params, ArrowFormat {4}));
	}
	{
		auto named = ParseOne(conn, "SELECT $base::BIGINT + i FROM range(10) t(i)");
		std::vector<NamedParam> params;
		params.push_back({"base", Value::Create(conn.GetFactory(), int64_t(10))});
		Check(conn.Execute(named, params, ArrowFormat {4}));
	}
}

TEST_CASE("Stable C++API: PreparedStatement executes into Arrow repeatedly", "[cpp_api][arrow][prepared_statement]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto statement = ParseOne(conn, "SELECT i + $1::BIGINT FROM range(10) t(i)");
	auto prepared = conn.Prepare(statement);

	for (int64_t base : {int64_t(0), int64_t(100)}) {
		std::vector<Value> params;
		params.push_back(Value::Create(conn.GetFactory(), base));
		auto result = prepared.Execute(params, ArrowFormat {4});

		// The result reports what the statement is, not EXECUTE.
		REQUIRE(result.GetStatementType() == StatementType::SELECT);

		ArrowBatches batches;
		FetchAll(result, batches);
		REQUIRE(batches.RowCount() == 10);
		REQUIRE(batches.MaxLength() <= 4);
		REQUIRE(CountsUpFrom(batches, base));
	}

	{
		auto value = Value::Create(conn.GetFactory(), int64_t(10));
		auto result = prepared.Execute(&value, 1, ArrowFormat {4});
		ArrowBatches batches;
		FetchAll(result, batches);
		REQUIRE(CountsUpFrom(batches, 10));
	}

	{
		auto named_statement = ParseOne(conn, "SELECT i + $base::BIGINT FROM range(10) t(i)");
		auto named_prepared = conn.Prepare(named_statement);
		std::vector<NamedParam> params;
		params.push_back({"base", Value::Create(conn.GetFactory(), int64_t(10))});
		ArrowBatches batches;
		auto result = named_prepared.Execute(params, ArrowFormat {4});
		FetchAll(result, batches);
		REQUIRE(CountsUpFrom(batches, 10));
	}
}

TEST_CASE("Stable C++API: ArrowResult drains side effects and reports shapes", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	{
		auto result = conn.Execute("CREATE TABLE t(x BIGINT)", ArrowFormat {});
		REQUIRE(result.GetResultType() == ResultType::NOTHING);
		REQUIRE(result.Drain() == 0);
	}
	{
		auto result = conn.Execute("INSERT INTO t SELECT * FROM range(1234)", ArrowFormat {});
		REQUIRE(result.GetResultType() == ResultType::CHANGED_ROWS);
		REQUIRE(result.GetStatementType() == StatementType::INSERT);
		REQUIRE(result.Drain() == 1234);
	}
	{
		auto statement = ParseOne(conn, "INSERT INTO t SELECT * FROM range($1::BIGINT)");
		auto prepared = conn.Prepare(statement);
		std::vector<Value> params;
		params.push_back(Value::Create(conn.GetFactory(), int64_t(17)));
		REQUIRE(prepared.Execute(params, ArrowFormat {}).Drain() == 17);
	}
}

TEST_CASE("Stable C++API: ArrowResult surfaces errors as exceptions", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	SECTION("a binding error throws at Execute and leaves the statement usable") {
		auto statement = ParseOne(conn, "SELECT * FROM missing_t");
		REQUIRE_THROWS_MATCHES(conn.Execute(statement, ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_DATABASE_CATALOG));
		conn.Execute("CREATE TABLE missing_t AS SELECT 1::BIGINT AS i").Drain();
		ArrowBatches batches;
		auto result = conn.Execute(statement, ArrowFormat {});
		FetchAll(result, batches);
		REQUIRE(batches.RowCount() == 1);
	}

	SECTION("a null parameter pointer with a nonzero count is refused, not read") {
		auto statement = ParseOne(conn, "SELECT $1::BIGINT");
		REQUIRE_THROWS_MATCHES(conn.Execute(statement, nullptr, 1, ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		REQUIRE_THROWS_MATCHES(conn.Execute(statement, nullptr, 1), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		auto prepared = conn.Prepare(statement);
		REQUIRE_THROWS_MATCHES(prepared.Execute(nullptr, 1, ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	SECTION("named-parameter mistakes are refused and leave the statement usable") {
		auto statement = ParseOne(conn, "SELECT $base::BIGINT + i FROM range(10) t(i)");

		std::vector<NamedParam> wrong;
		wrong.push_back({"wrong", Value::Create(conn.GetFactory(), int64_t(10))});
		REQUIRE_THROWS_MATCHES(conn.Execute(statement, wrong, ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		auto prepared = conn.Prepare(statement);
		REQUIRE_THROWS_MATCHES(prepared.Execute(wrong, ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

		// A statement cannot mix named and positional parameters.
		auto two = ParseOne(conn, "SELECT $a::BIGINT + $b::BIGINT");
		std::vector<NamedParam> mixed;
		mixed.push_back({"a", Value::Create(conn.GetFactory(), int64_t(1))});
		mixed.push_back({"", Value::Create(conn.GetFactory(), int64_t(2))});
		REQUIRE_THROWS_MATCHES(conn.Execute(two, mixed, ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

		// The failures left both handles usable.
		std::vector<NamedParam> right;
		right.push_back({"base", Value::Create(conn.GetFactory(), int64_t(10))});
		ArrowBatches batches;
		auto result = prepared.Execute(right, ArrowFormat {});
		FetchAll(result, batches);
		REQUIRE(CountsUpFrom(batches, 10));
	}

	SECTION("the string form takes exactly one statement") {
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1; SELECT 2", ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		REQUIRE_THROWS_MATCHES(conn.Execute("", ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	SECTION("an execution error is sticky and leaves `out` released") {
		{
			auto result = conn.Execute("SELECT error('boom arrow') FROM range(10)", ArrowFormat {});
			ArrowArray array {};
			array.release = SentinelRelease;
			REQUIRE_THROWS_WITH(result.FetchArray(array), Catch::Contains("boom arrow"));
			REQUIRE(array.release == nullptr);
			// Rethrows carry the same error, not just any failure.
			array.release = SentinelRelease;
			REQUIRE_THROWS_WITH(result.FetchArray(array), Catch::Contains("boom arrow"));
			REQUIRE(array.release == nullptr);
			array.release = SentinelRelease;
			REQUIRE_THROWS_WITH(result.Step(array), Catch::Contains("boom arrow"));
			REQUIRE(array.release == nullptr);
		}
		// The failed result freed the connection.
		REQUIRE(conn.Execute("SELECT 1").Drain() == 0);
	}

	SECTION("Interrupt cancels: a step status and a FetchArray exception") {
		auto result = conn.Execute("SELECT i FROM range(10000000) t(i)", ArrowFormat {});
		// A deferred, never-stepped result already counts as the running query, so an
		// interrupt before the first Step is not a no-op. This test pins that.
		conn.Interrupt();

		auto status = StepStatus::WAITING;
		for (int i = 0; i < 1000 && status != StepStatus::CANCELLED; i++) {
			ArrowArray array {};
			status = result.Step(array);
			if (array.release) {
				array.release(&array);
			}
		}
		REQUIRE(status == StepStatus::CANCELLED);

		// CANCELLED is sticky, and `out` is reset even when no array comes.
		ArrowArray array {};
		array.release = SentinelRelease;
		REQUIRE(result.Step(array) == StepStatus::CANCELLED);
		REQUIRE(array.release == nullptr);

		array.release = SentinelRelease;
		REQUIRE_THROWS_MATCHES(result.FetchArray(array), Exception, HasErrorCode(DUCKDB_V2_ERROR_RUNTIME_INTERRUPT));
		REQUIRE(array.release == nullptr);
		REQUIRE_THROWS_MATCHES(result.Drain(), Exception, HasErrorCode(DUCKDB_V2_ERROR_RUNTIME_INTERRUPT));

		// The cancelled result freed the connection.
		REQUIRE(conn.Execute("SELECT 1").Drain() == 0);
	}
}

TEST_CASE("Stable C++API: ToArrowStream consumes the result and continues it", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	SECTION("the stream picks up where FetchArray left off") {
		auto result = conn.Execute("SELECT i FROM range(10000) t(i)", ArrowFormat {1000});

		OwnedSchema schema;
		result.GetSchema(schema.schema);

		ArrowArray first {};
		REQUIRE(result.FetchArray(first));
		auto already_read = first.length;
		REQUIRE(Int64At(first, 0, 0) == 0);
		first.release(&first);

		OwnedStream stream;
		result.ToArrowStream(stream.stream);
		REQUIRE_FALSE(static_cast<bool>(result));

		OwnedSchema stream_schema;
		REQUIRE(stream.stream.get_schema(&stream.stream, &stream_schema.schema) == 0);
		REQUIRE(ChildNames(stream_schema.schema) == ChildNames(schema.schema));

		ArrowBatches rest;
		REQUIRE(StreamAll(stream.stream, rest) == 0);
		REQUIRE(rest.RowCount() == 10000 - static_cast<idx_t>(already_read));
		REQUIRE(CountsUpFrom(rest, already_read));
	}

	SECTION("the stream keeps the connection busy until it is released") {
		OwnedStream stream;
		{
			auto result = conn.Execute("SELECT i FROM range(10000) t(i)", ArrowFormat {});
			result.ToArrowStream(stream.stream);
			// Leaving the scope destroys the empty wrapper, not the result.
		}
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1"), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
		stream.stream.release(&stream.stream);
		REQUIRE(conn.Execute("SELECT 1").Drain() == 0);
	}

	SECTION("a temporary result converts and the stream outlives it") {
		OwnedStream stream;
		conn.Execute("SELECT 42::BIGINT AS answer", ArrowFormat {}).ToArrowStream(stream.stream);
		ArrowBatches batches;
		REQUIRE(StreamAll(stream.stream, batches) == 0);
		REQUIRE(batches.RowCount() == 1);
		REQUIRE(Int64At(batches.arrays[0], 0, 0) == 42);
	}
}

TEST_CASE("Stable C++API: one live result per connection, in either format", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto statement = ParseOne(conn, "SELECT 1");
	auto prepared = conn.Prepare(statement);

	{
		auto arrow_result = conn.Execute("SELECT i FROM range(10) t(i)", ArrowFormat {});
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1"), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1", ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
		REQUIRE_THROWS_MATCHES(prepared.Execute(ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
	}
	{
		auto chunk_result = conn.Execute("SELECT i FROM range(10) t(i)");
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1", ArrowFormat {}), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
	}

	// Leaving each scope freed the connection, and the refused prepared statement stayed usable.
	REQUIRE(conn.Execute("SELECT 1", ArrowFormat {}).Drain() == 0);
	REQUIRE(prepared.Execute(ArrowFormat {}).Drain() == 0);
}

TEST_CASE("Stable C++API: move assignment swaps two live ArrowResults", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn1 = db.Connect();
	auto conn2 = db.Connect();

	auto held = conn1.Execute("SELECT 1", ArrowFormat {});
	{
		auto incoming = conn2.Execute("SELECT 2::BIGINT", ArrowFormat {});
		held = std::move(incoming);
		// Move assignment swaps: the moved-from wrapper holds the old result,
		// so both connections stay busy while both wrappers live.
		REQUIRE(static_cast<bool>(held));
		REQUIRE(static_cast<bool>(incoming));
		REQUIRE_THROWS_MATCHES(conn1.Execute("SELECT 1"), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
		REQUIRE_THROWS_MATCHES(conn2.Execute("SELECT 1"), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
	}
	// `incoming` died holding conn1's old result; conn2's result lives on in `held`.
	REQUIRE(conn1.Execute("SELECT 1").Drain() == 0);
	REQUIRE_THROWS_MATCHES(conn2.Execute("SELECT 1"), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));

	ArrowBatches batches;
	FetchAll(held, batches);
	REQUIRE(batches.RowCount() == 1);
	REQUIRE(Int64At(batches.arrays[0], 0, 0) == 2);
	REQUIRE(conn2.Execute("SELECT 1").Drain() == 0);
}

TEST_CASE("Stable C++API: a moved-from ArrowResult is empty and safe", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto source = conn.Execute("SELECT i FROM range(10) t(i)", ArrowFormat {});
	auto target = std::move(source);
	REQUIRE_FALSE(static_cast<bool>(source));
	REQUIRE(static_cast<bool>(target));

	// Every method on the empty wrapper throws, and `out` ends up released.
	ArrowArray array {};
	array.release = SentinelRelease;
	REQUIRE_THROWS_MATCHES(source.FetchArray(array), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(array.release == nullptr);
	array.release = SentinelRelease;
	REQUIRE_THROWS_MATCHES(source.Step(array), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(array.release == nullptr);
	OwnedSchema schema;
	schema.schema.release = SentinelSchemaRelease;
	REQUIRE_THROWS_MATCHES(source.GetSchema(schema.schema), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(schema.schema.release == nullptr);
	REQUIRE_THROWS_MATCHES(source.Drain(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(source.Wait(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(source.GetResultType(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(source.GetStatementType(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	OwnedStream stream;
	stream.stream.release = SentinelStreamRelease;
	REQUIRE_THROWS_MATCHES(source.ToArrowStream(stream.stream), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(stream.stream.release == nullptr);

	// The handle moved with the wrapper: the target reads every row.
	ArrowBatches batches;
	FetchAll(target, batches);
	REQUIRE(batches.RowCount() == 10);
	REQUIRE(CountsUpFrom(batches, 0));
}
