#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"
#include "test_helpers.hpp"

// Internal engine headers: used only to construct a DICTIONARY vector, which
// no public surface builds directly. Tests are the sanctioned mixing point.
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Stable C++ API tests: the vector read and write surface.
// ---------------------------------------------------------------------------

namespace {

// Read a transparent duckdb_v2_bytes slot's bytes (inlined or heap form).
// Mirrors V2StringView in capi_v2_test_helpers.hpp; kept separate because
// the C++ suite does not include the V1-including C helpers header.
std::string SlotBytes(const duckdb_v2_bytes &s) {
	uint32_t len = s.value.inlined.length;
	const char *ptr = len <= DUCKDB_V2_BYTES_INLINE_LENGTH ? s.value.inlined.inlined : s.value.pointer.ptr;
	return std::string(ptr, len);
}

} // namespace

TEST_CASE("Stable C++API: Vector AssignString", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(3);

	// AssignString resolves the heap once and reuses it for the rest.
	vec.AssignString(0, "hi"); // inlined
	const std::string long_str(100, 'x');
	vec.AssignString(1, long_str); // copied into the heap
	vec.AssignString(2, "");       // empty

	// The cpp_api has no VARCHAR read path yet; read the transparent
	// duckdb_v2_bytes slots directly to confirm the bytes round-trip.
	auto *slots = vec.GetDataMutable<duckdb_v2_bytes>();
	REQUIRE(SlotBytes(slots[0]) == "hi");
	REQUIRE(SlotBytes(slots[1]) == long_str);
	REQUIRE(SlotBytes(slots[2]).empty());
}
TEST_CASE("Stable C++API: Vector AssignString over a batch of slots", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(3);

	// A single write, then a bulk write starting at index 1; both share the
	// cached heap. The bulk batch mixes an inlined and a heap-allocated value.
	vec.AssignString(0, "head");
	const std::vector<std::string> owned = {"second", "this tail value is comfortably longer than twelve bytes"};
	const std::vector<std::string_view> tail(owned.begin(), owned.end());
	for (idx_t i = 0; i < tail.size(); i++) {
		vec.AssignString(1 + i, tail[i]);
	}

	auto *slots = vec.GetDataMutable<duckdb_v2_bytes>();
	REQUIRE(SlotBytes(slots[0]) == "head");
	REQUIRE(SlotBytes(slots[1]) == owned[0]);
	REQUIRE(SlotBytes(slots[2]) == owned[1]);
}
TEST_CASE("Stable C++API: StringHeap primitive (dedup + scatter)", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(4);

	auto heap = vec.GetHeap();

	// Dedup: intern a (non-inlined) value once, reference it from many slots.
	const std::string shared_str = "this is a shared value, longer than twelve bytes";
	auto shared = heap.AddString(shared_str);
	vec.SetString(0, shared);
	vec.SetString(2, shared);

	// Intern each, then scatter the tokens into arbitrary positions.
	const std::vector<std::string> owned = {"x", "another longer-than-inline string value"};
	std::vector<varchar_t> tokens;
	for (const auto &value : owned) {
		tokens.push_back(heap.AddString(value));
	}
	vec.SetString(3, tokens[0]);
	vec.SetString(1, tokens[1]);

	auto *slots = vec.GetDataMutable<duckdb_v2_bytes>();
	REQUIRE(SlotBytes(slots[0]) == shared_str);
	REQUIRE(SlotBytes(slots[1]) == owned[1]);
	REQUIRE(SlotBytes(slots[2]) == shared_str);
	REQUIRE(SlotBytes(slots[3]) == "x");
}
TEST_CASE("Stable C++API: StringHeap::Allocate write-in-place", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(2);

	auto heap = vec.GetHeap();

	// Write-in-place: generate bytes straight into the heap, then build a token over them.
	const uint32_t len = 64;
	auto *bytes = heap.Allocate(len);
	REQUIRE(bytes != nullptr);
	std::memset(bytes, 'q', len);
	varchar_t token(reinterpret_cast<char *>(bytes), len);
	REQUIRE_FALSE(token.is_inlined());
	REQUIRE(token.size() == len);
	REQUIRE(token.data() == reinterpret_cast<const char *>(bytes));
	vec.SetString(0, token);

	// Inlined token: the bytes live in the value itself.
	auto small = heap.AddString("tiny");
	REQUIRE(small.is_inlined());
	REQUIRE(small.size() == 4);
	REQUIRE(std::string(small.data(), small.size()) == "tiny");
	vec.SetString(1, small);

	auto *slots = vec.GetDataMutable<duckdb_v2_bytes>();
	REQUIRE(SlotBytes(slots[0]) == std::string(len, 'q'));
	REQUIRE(SlotBytes(slots[1]) == "tiny");
}
TEST_CASE("Stable C++API: AssignString rejects misuse", "[cpp_api]") {
	using namespace duckdb::cxx;

	// A non-string vector has no heap: AssignString surfaces INVALID_INPUT.
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		auto &factory = conn.GetFactory();
		std::vector<LogicalType> types;
		types.push_back(factory.ParseType("INTEGER"));
		DataChunk chunk(factory, types);
		auto vec = chunk.GetVector(0);
		vec.SetSize(1);
		REQUIRE_THROWS_MATCHES(vec.AssignString(0, "x"), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// A CONSTANT vector's data array holds one slot: only index 0 is writable.
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		auto &factory = conn.GetFactory();
		std::vector<LogicalType> types;
		types.push_back(factory.ParseType("VARCHAR"));
		DataChunk chunk(factory, types);
		auto vec = chunk.GetVector(0);
		vec.MakeConstant(Value::Create(factory, varchar_t("const")), 2);
		REQUIRE_THROWS_MATCHES(vec.AssignString(1, "x"), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		REQUIRE_NOTHROW(vec.AssignString(0, "ok"));
	}
}
#if (STANDARD_VECTOR_SIZE > 10)
TEST_CASE("Stable C++API: VectorView NULL-aware read of a queried chunk", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto result = conn.Execute("SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i END AS v FROM range(10) t(i)");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);

	auto vec = chunk.GetVector(0);
	REQUIRE(vec.GetVectorType() == VectorType::FLAT);

	auto view = vec.GetView();
	REQUIRE(view.count == 10);
	REQUIRE(view.sel == nullptr); // FLAT: identity
	auto data = view.Data<int64_t>();
	for (idx_t i = 0; i < view.count; i++) {
		if (i % 3 == 0) {
			REQUIRE_FALSE(view.IsValid(i));
			continue;
		}
		REQUIRE(view.IsValid(i));
		REQUIRE(data[view.SelAt(i)] == static_cast<int64_t>(i));
	}
}
#endif

#if (STANDARD_VECTOR_SIZE > 3)
TEST_CASE("Stable C++API: VectorView CONSTANT without flatten", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);

	vec.MakeConstant(Value::Create(factory, int64_t(7)), 4);
	REQUIRE(vec.GetVectorType() == VectorType::CONSTANT);

	auto view = vec.GetView();
	REQUIRE(view.count == 4);
	REQUIRE(view.sel != nullptr); // zero singleton, not identity
	auto data = view.Data<int64_t>();
	for (idx_t i = 0; i < view.count; i++) {
		REQUIRE(view.SelAt(i) == 0); // every row resolves to the one slot
		REQUIRE(view.IsValid(i));
		REQUIRE(data[view.SelAt(i)] == 7);
	}
	// The view did not flatten.
	REQUIRE(vec.GetVectorType() == VectorType::CONSTANT);
}
#endif

#if (STANDARD_VECTOR_SIZE > 3)
TEST_CASE("Stable C++API: VectorView DICTIONARY resolves validity through sel", "[cpp_api]") {
	using namespace duckdb::cxx;

	// Built via core (the vector handle is identity = duckdb::Vector *): a
	// FLAT dictionary with physical row 1 NULL, sliced so logical row 3
	// dispatches to it.
	duckdb::Vector flat(duckdb::LogicalType::INTEGER);
	auto *fd = duckdb::FlatVector::GetDataMutable<int32_t>(flat);
	fd[0] = 10;
	fd[1] = 20;
	fd[2] = 30;
	duckdb::FlatVector::SetNull(flat, 1, true);

	duckdb::SelectionVector sel(4);
	sel.set_index(0, 2);
	sel.set_index(1, 0);
	sel.set_index(2, 2);
	sel.set_index(3, 1);
	duckdb::Vector dict(flat, sel, 4);

	auto vec = detail::HandleFactory::Make<Vector>(reinterpret_cast<duckdb_v2_vector_handle>(&dict));
	REQUIRE(vec.GetVectorType() == VectorType::DICTIONARY);

	auto view = vec.GetView();
	REQUIRE(view.count == 4);
	REQUIRE(view.sel != nullptr); // the dictionary's own sel
	auto data = view.Data<int32_t>();

	// Reads dispatch through sel.
	REQUIRE(view.SelAt(0) == 2);
	REQUIRE(data[view.SelAt(0)] == 30);
	REQUIRE(data[view.SelAt(1)] == 10);
	REQUIRE(data[view.SelAt(2)] == 30);

	// Validity follows sel: logical row 3 is NULL (physical 1), while the
	// naive physical read of row 3 would answer valid.
	REQUIRE_FALSE(view.IsValid(3));
	REQUIRE(view.RowIsValid(3));
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE(view.IsValid(i));
	}

	// The view did not flatten the parent.
	REQUIRE(vec.GetVectorType() == VectorType::DICTIONARY);
}
#endif

#if (STANDARD_VECTOR_SIZE > 3)
TEST_CASE("Stable C++API: MakeSequence and MakeConstant round-trip", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	types.push_back(factory.ParseType("BIGINT"));
	DataChunk chunk(factory, types);

	// A SEQUENCE reads as Other; Flatten materialises it to FLAT.
	auto seq = chunk.GetVector(0);
	seq.MakeSequence(100, 2, 4);
	REQUIRE(seq.GetVectorType() == VectorType::OTHER);
	seq.Flatten();
	REQUIRE(seq.GetVectorType() == VectorType::FLAT);
	auto view = seq.GetView();
	REQUIRE(view.count == 4);
	auto data = view.Data<int64_t>();
	for (idx_t i = 0; i < view.count; i++) {
		REQUIRE(data[view.SelAt(i)] == 100 + 2 * static_cast<int64_t>(i));
	}

	// A CONSTANT holds one slot referenced by every logical row.
	auto con = chunk.GetVector(1);
	con.MakeConstant(Value::Create(factory, int64_t(-5)), 3);
	REQUIRE(con.GetVectorType() == VectorType::CONSTANT);
	auto cview = con.GetView();
	REQUIRE(cview.count == 3);
	REQUIRE(cview.Data<int64_t>()[cview.SelAt(2)] == -5);
}
#endif

#if (STANDARD_VECTOR_SIZE > 2)
#ifndef DUCKDB_DEBUG_NO_INLINE
TEST_CASE("Stable C++API: VectorView VARCHAR and BLOB reads via blob_t", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	const std::string long_str = "this string is comfortably longer than twelve bytes";
	auto result = conn.Execute("SELECT v, v::BLOB AS b FROM (VALUES ('tiny'), ('" + long_str + "'), (NULL)) t(v)");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);

	auto view = chunk.GetVector(0).GetView();
	REQUIRE(view.count == 3);
	auto strings = view.Data<blob_t>();

	const auto &small = strings[view.SelAt(0)];
	REQUIRE(view.IsValid(0));
	REQUIRE(small.is_inlined());
	REQUIRE(small.view() == "tiny");

	const auto &large = strings[view.SelAt(1)];
	REQUIRE(view.IsValid(1));
	REQUIRE_FALSE(large.is_inlined());
	REQUIRE(large.view() == long_str);

	REQUIRE_FALSE(view.IsValid(2));

	// BLOB shares the storage layout; the bytes read the same way.
	auto bview = chunk.GetVector(1).GetView();
	auto blobs = bview.Data<blob_t>();
	REQUIRE(blobs[bview.SelAt(0)].view() == "tiny");
	REQUIRE(blobs[bview.SelAt(1)].view() == long_str);
	REQUIRE_FALSE(bview.IsValid(2));
}
#endif
#endif

#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("Stable C++API: validity write round-trip", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	types.push_back(factory.ParseType("BIGINT"));
	DataChunk chunk(factory, types);

	// FLAT: ValidityMask writes are observed by the read view.
	auto vec = chunk.GetVector(0);
	vec.SetSize(3);
	auto data = vec.GetDataMutable<int32_t>();
	data[0] = 1;
	data[1] = 2;
	data[2] = 3;

	auto mask = vec.GetValidityMutable();
	REQUIRE(mask.words != nullptr);
	mask.SetInvalid(1);
	REQUIRE(mask.RowIsValid(0));
	REQUIRE_FALSE(mask.RowIsValid(1));

	auto view = vec.GetView();
	REQUIRE(view.IsValid(0));
	REQUIRE_FALSE(view.IsValid(1));
	REQUIRE(view.IsValid(2));

	mask.SetValid(1);
	REQUIRE(vec.GetView().IsValid(1));

	// CONSTANT: SetConstantValid flips the single bit for every row.
	auto con = chunk.GetVector(1);
	con.MakeConstant(Value::Create(factory, int64_t(9)), 4);
	con.SetConstantValid(false);
	auto cview = con.GetView();
	for (idx_t i = 0; i < 4; i++) {
		REQUIRE_FALSE(cview.IsValid(i));
	}
	con.SetConstantValid(true);
	REQUIRE(con.GetView().IsValid(0));
}
#endif

TEST_CASE("Stable C++API: validity mask word-boundary rows", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(130); // spans three 64-row validity words

	// Clear the bits adjacent to both word boundaries (63|64 and 127|128).
	auto mask = vec.GetValidityMutable();
	for (idx_t row : {idx_t(63), idx_t(64), idx_t(127), idx_t(128)}) {
		mask.SetInvalid(row);
		REQUIRE_FALSE(mask.RowIsValid(row));
	}
	// Neighbours in the adjacent words are untouched: no cross-word bleed.
	for (idx_t row : {idx_t(0), idx_t(62), idx_t(65), idx_t(126), idx_t(129)}) {
		REQUIRE(mask.RowIsValid(row));
	}

	// A fresh view observes the same bits through VectorView's own bit math.
	auto view = vec.GetView();
	REQUIRE(view.count == 130);
	for (idx_t row : {idx_t(63), idx_t(64), idx_t(127), idx_t(128)}) {
		REQUIRE_FALSE(view.IsValid(row));
	}
	for (idx_t row : {idx_t(0), idx_t(62), idx_t(65), idx_t(126), idx_t(129)}) {
		REQUIRE(view.IsValid(row));
	}

	// Flip one bit per word back; its boundary partner stays invalid.
	mask.SetValid(64);
	mask.SetValid(127);
	auto reread = vec.GetView();
	REQUIRE(reread.IsValid(64));
	REQUIRE(reread.IsValid(127));
	REQUIRE_FALSE(reread.IsValid(63));
	REQUIRE_FALSE(reread.IsValid(128));
}
TEST_CASE("Stable C++API: VectorView::AllValid ignores bits past the row count", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(70); // a full word plus a 6-row tail

	// Without a mask, the view reports all valid.
	REQUIRE(vec.GetView().AllValid());

	// Rebuild the mask the born-invalid way: SetAllInvalid also clears the bits past row 69 in the tail word, so an
	// all-valid verdict must not depend on them.
	auto mask = vec.GetValidityMutable();
	mask.SetAllInvalid(70);
	REQUIRE_FALSE(vec.GetView().AllValid());
	for (idx_t row = 0; row < 70; row++) {
		mask.SetValid(row);
	}
	REQUIRE(vec.GetView().AllValid());

	// One NULL row, in the tail word, flips the verdict.
	mask.SetInvalid(69);
	REQUIRE_FALSE(vec.GetView().AllValid());
}
TEST_CASE("Stable C++API: vector read surface rejects misuse", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	types.push_back(factory.ParseType("BIGINT"));
	DataChunk chunk(factory, types);

	// GetView rejects VectorType::OTHER (a SEQUENCE) until flattened.
	auto seq = chunk.GetVector(0);
	seq.MakeSequence(0, 1, 4);
	REQUIRE(seq.GetVectorType() == VectorType::OTHER);
	REQUIRE_THROWS_MATCHES(seq.GetView(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// GetValidityMutable is FLAT-only.
	auto con = chunk.GetVector(1);
	con.MakeConstant(Value::Create(factory, int64_t(1)), 2);
	REQUIRE_THROWS_MATCHES(con.GetValidityMutable(), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// SetConstantValid is CONSTANT-only.
	seq.Flatten();
	REQUIRE_THROWS_MATCHES(seq.SetConstantValid(true), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: Vector SetNull recurses into nested children", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("STRUCT(name VARCHAR, score DOUBLE)"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(3);
	REQUIRE(chunk.GetVectorCount() == 1);
	REQUIRE(vec.GetChildCount() == 2);

	auto name_vec = vec.GetChild(0);
	auto score_vec = vec.GetChild(1);
	auto *scores = score_vec.GetDataMutable<double>();
	for (idx_t i = 0; i < 3; i++) {
		name_vec.AssignString(i, "n");
		scores[i] = static_cast<double>(i);
	}

	vec.SetNull(1);

	// The NULL broadcasts into both field slots; other rows keep values.
	REQUIRE_FALSE(vec.GetView().RowIsValid(1));
	REQUIRE_FALSE(name_vec.GetView().RowIsValid(1));
	REQUIRE_FALSE(score_vec.GetView().RowIsValid(1));
	REQUIRE(vec.GetView().RowIsValid(0));
	REQUIRE(name_vec.GetView().RowIsValid(2));
	REQUIRE(score_vec.GetView().Data<double>()[2] == 2.0);

	// FLAT-only and bounds-checked, matching the C contract.
	REQUIRE_THROWS_MATCHES(vec.SetNull(3), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: ValidityMask SetAllInvalid born-invalid pattern", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	// Cross a word boundary so more than one mask word is cleared.
	vec.SetSize(70);

	// Born-invalid: clear everything up front, then earn validity by
	// writing. Rows never written stay NULL without further bookkeeping.
	auto validity = vec.GetValidityMutable();
	validity.SetAllInvalid(70);

	vec.AssignString(3, "three");
	validity.SetValid(3);
	vec.AssignString(64, "sixty-four");
	validity.SetValid(64);

	auto view = vec.GetView();
	for (idx_t i = 0; i < 70; i++) {
		REQUIRE(view.RowIsValid(i) == (i == 3 || i == 64));
	}
}

TEST_CASE("Stable C++API: ValidityMask SetAllValid born-valid and reset", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	// Cross a word boundary so more than one mask word is touched.
	vec.SetSize(70);
	auto *data = vec.GetDataMutable<int32_t>();
	auto validity = vec.GetValidityMutable();

	// Born-valid: mark everything valid, then only clear the nulls. Fewer
	// writes than born-invalid when most rows carry a value.
	validity.SetAllValid(70);
	for (idx_t i = 0; i < 70; i++) {
		data[i] = static_cast<int32_t>(i);
	}
	validity.SetInvalid(5);
	validity.SetInvalid(64);

	auto view = vec.GetView();
	for (idx_t i = 0; i < 70; i++) {
		REQUIRE(view.RowIsValid(i) == (i != 5 && i != 64));
	}

	// Reuse: SetAllValid restores every row, clearing the two nulls from the
	// previous fill without touching them one by one.
	validity.SetAllValid(70);
	auto reset_view = vec.GetView();
	for (idx_t i = 0; i < 70; i++) {
		REQUIRE(reset_view.RowIsValid(i));
	}
}

TEST_CASE("Stable C++API: checked and unsafe UTF-8 string construction", "[cpp_api]") {
	using namespace duckdb::cxx;
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));
	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(3);
	auto heap = vec.GetHeap();
	auto slots = vec.GetDataMutable<varchar_t>();

	const std::string valid[] = {"", "ASCII", "é🦆", std::string("a\0b", 3), "🦆🦆🦆🦆"};
	for (const auto &text : valid) {
		REQUIRE_NOTHROW(ValidateUTF8(text));
		vec.AssignString(0, text);
		vec.SetString(1, heap.AddString(text));
		vec.SetString(2, heap.AddStringUnsafe(text));
		for (idx_t i = 0; i < 3; i++) {
			REQUIRE(slots[i].view() == text);
		}
	}

	const std::string malformed[] = {"\xFF", std::string("a\0\xFF", 3), std::string(40, '\xFF')};
	for (const auto &text : malformed) {
		REQUIRE_THROWS_MATCHES(ValidateUTF8(text), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		REQUIRE_THROWS_MATCHES(heap.AddString(text), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

		vec.AssignString(0, "🦆");
		REQUIRE_THROWS_MATCHES(vec.AssignString(0, text), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		REQUIRE(slots[0].view() == "🦆");
		vec.AssignStringUnsafe(1, text);
		vec.SetString(2, heap.AddStringUnsafe(text));
		REQUIRE(slots[1].view() == text);
		REQUIRE(slots[2].view() == text);
	}
}

TEST_CASE("Stable C++API: AssignString preserves binary bytes", "[cpp_api]") {
	using namespace duckdb::cxx;
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto bignum = bignum_t::Encode({{0xFF}, false});
	const std::pair<const char *, std::string> values[] = {
	    {"BLOB", "\xFF"}, {"BIT", std::string("\0\xFF", 2)}, {"BIGNUM", std::string(bignum.begin(), bignum.end())}};
	for (const auto &value : values) {
		std::vector<LogicalType> types;
		types.push_back(factory.ParseType(value.first));
		DataChunk chunk(factory, types);
		auto vec = chunk.GetVector(0);
		vec.SetSize(1);
		vec.AssignString(0, value.second);
		REQUIRE(vec.GetDataMutable<blob_t>()[0].view() == value.second);
	}
}

#if (STANDARD_VECTOR_SIZE > 3)
TEST_CASE("Stable C++API: Vector Reference aliases the source without copying", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	types.push_back(factory.ParseType("VARCHAR[]"));
	types.push_back(factory.ParseType("VARCHAR"));
	DataChunk source(factory, types);
	DataChunk target(factory, types);

	auto src = source.GetVector(0);
	src.SetSize(4);
	auto *src_data = src.GetDataMutable<int64_t>();
	for (idx_t i = 0; i < 4; i++) {
		src_data[i] = static_cast<int64_t>(i * 10);
	}
	src.SetNull(2);

	auto dst = target.GetVector(0);
	dst.SetSize(1);
	dst.Reference(src);

	// Same buffer, same size, same validity: no copy happened.
	auto view = dst.GetView();
	REQUIRE(dst.GetSize() == 4);
	REQUIRE(view.count == 4);
	REQUIRE(view.Data<int64_t>() == src_data);
	REQUIRE(view.Data<int64_t>()[3] == 30);
	REQUIRE_FALSE(view.RowIsValid(2));

	// A write through the source shows up in the referencing vector.
	src_data[0] = -1;
	REQUIRE(dst.GetValue(0).Get<int64_t>() == -1);

	// A CONSTANT source is referenced as a CONSTANT, not materialized.
	auto csrc = source.GetVector(2);
	csrc.MakeConstant(Value::Create(factory, varchar_t("c")), 3);
	auto cdst = target.GetVector(2);
	cdst.Reference(csrc);
	REQUIRE(cdst.GetVectorType() == VectorType::CONSTANT);
	REQUIRE(cdst.GetSize() == 3);
	REQUIRE(cdst.GetValue(2).Get<varchar_t>().view() == "c");

	// Nested: the list entries and the child vector come along.
	auto lsrc = source.GetVector(1);
	lsrc.SetSize(2);
	std::vector<Value> first;
	first.push_back(Value::Create(factory, varchar_t("a")));
	first.push_back(Value::Create(factory, varchar_t("b")));
	std::vector<Value> second;
	second.push_back(Value::Create(factory, varchar_t("z")));
	lsrc.SetValue(0, Value::CreateList(factory, first));
	lsrc.SetValue(1, Value::CreateList(factory, second));
	auto ldst = target.GetVector(1);
	ldst.Reference(lsrc);
	REQUIRE(ldst.GetSize() == 2);
	REQUIRE(ldst.GetValue(0).GetChildCount() == 2);
	REQUIRE(ldst.GetValue(0).GetChild(1).Get<varchar_t>().view() == "b");
	REQUIRE(ldst.GetValue(1).GetChildCount() == 1);
	REQUIRE(ldst.GetChild(0).GetValue(2).Get<varchar_t>().view() == "z");

	// The types must match.
	REQUIRE_THROWS_MATCHES(dst.Reference(csrc), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(ldst.Reference(src), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	// The failed call left the vector untouched.
	REQUIRE(dst.GetSize() == 4);
	REQUIRE(dst.GetValue(3).Get<int64_t>() == 30);
}
#endif

TEST_CASE("Stable C++API: MAP entries are written through its entries child", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE maps (m MAP(INTEGER, VARCHAR))").Drain();

	Appender appender(conn, "maps");
	DataChunk chunk(factory, appender.ColumnTypes());
	auto map_vec = chunk.GetVector(0);
	map_vec.SetSize(1);
	REQUIRE(map_vec.GetChildCount() == 1);
	// sizing the entries sizes both the keys and the values
	auto entries = map_vec.GetChild(0);
	entries.SetSize(2);
	REQUIRE(entries.GetChildCount() == 2);
	auto keys = entries.GetChild(0);
	auto values = entries.GetChild(1);

	auto *key_data = keys.GetDataMutable<int32_t>();
	key_data[0] = 1;
	key_data[1] = 2;
	// Longer than blob_t::INLINE_LENGTH: an engine built with DUCKDB_DEBUG_NO_INLINE reads the pointer of any string
	values.AssignString(0, "the first value");
	values.AssignString(1, "the second value");
	map_vec.GetDataMutable<duckdb_v2_list_entry>()[0] = {0, 2};
	appender.AppendChunk(chunk);
	appender.Flush();

	auto result = conn.Execute("SELECT m::VARCHAR FROM maps");
	auto out = result.FetchChunk();
	REQUIRE(out);
	REQUIRE(out.GetVector(0).GetValue(0).Get<varchar_t>().view() == "{1=the first value, 2=the second value}");
	REQUIRE_THROWS_MATCHES(map_vec.GetChild(1), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: DataChunk capacity", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	DataChunk chunk(factory, types);
	REQUIRE(chunk.GetCapacity() == STANDARD_VECTOR_SIZE);
	REQUIRE(chunk.GetRowCount() == 0);
}

TEST_CASE("Stable C++API: vector cast and try cast", "[cpp_api]") {
	using namespace duckdb::cxx;
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));
	types.push_back(factory.ParseType("INTEGER"));
	DataChunk chunk(factory, types);
	auto source = chunk.GetVector(0);
	auto target = chunk.GetVector(1);
	// Padded past the inline string length: a build without string inlining reads the pointer of every string, and
	// the API leaves it unset for the strings it inlines. The integer cast skips the leading spaces.
	const std::string pad(16, ' ');
	source.SetSize(3);
	source.AssignString(0, pad + "1");
	source.AssignString(1, pad + "x");
	source.AssignString(2, pad + "3");

	REQUIRE_THROWS_MATCHES(source.Cast(factory, target), Exception, HasErrorCode(DUCKDB_V2_ERROR_TYPE_CONVERSION));

	REQUIRE_FALSE(source.TryCast(factory, target));
	auto view = target.GetView();
	REQUIRE(view.count == 3);
	REQUIRE(view.IsValid(0));
	REQUIRE(view.Data<int32_t>()[view.SelAt(0)] == 1);
	REQUIRE_FALSE(view.IsValid(1));
	REQUIRE(view.Data<int32_t>()[view.SelAt(2)] == 3);

	// The error of the failed row is kept on request.
	std::string error;
	REQUIRE_FALSE(source.TryCast(factory, target, &error));
	REQUIRE_THAT(error, Catch::Matchers::Contains("x"));

	source.AssignString(1, pad + "2");
	source.Cast(factory, target);
	REQUIRE(target.GetView().Data<int32_t>()[1] == 2);
	error.clear();
	REQUIRE(source.TryCast(factory, target, &error));
	REQUIRE(error.empty());
}

TEST_CASE("Stable C++API: vector logical type and chunk clear", "[cpp_api]") {
	using namespace duckdb::cxx;
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	types.push_back(factory.ParseType("STRUCT(a VARCHAR)"));
	DataChunk chunk(factory, types);
	REQUIRE(chunk.GetVector(0).GetLogicalType().GetTypeId() == LogicalTypeId::INTEGER);
	REQUIRE(chunk.GetVector(1).GetLogicalType().ToText() == "STRUCT(a VARCHAR)");

	// A cleared chunk is empty and writable again, also after its vector was made CONSTANT.
	auto vec = chunk.GetVector(0);
	vec.SetSize(3);
	vec.GetDataMutable<int32_t>()[0] = 7;
	chunk.GetVector(1).SetSize(3);
	REQUIRE(chunk.GetRowCount() == 3);
	vec.MakeConstant(Value::Create(factory, int32_t {5}), 3);
	REQUIRE(vec.GetVectorType() == VectorType::CONSTANT);
	chunk.Clear();
	REQUIRE(chunk.GetRowCount() == 0);
	REQUIRE(vec.GetVectorType() == VectorType::FLAT);
	REQUIRE(vec.GetLogicalType().GetTypeId() == LogicalTypeId::INTEGER);
	vec.SetSize(1);
	vec.GetDataMutable<int32_t>()[0] = 9;
	REQUIRE(vec.GetView().Data<int32_t>()[0] == 9);
}
