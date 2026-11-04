#include "test_capi_v2.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 vector_cast tests: cast a VARCHAR column to INTEGER in both cast modes,
// through a connection's and an instance's factory.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// Pads a cell past the inline string length: a build without string inlining reads the pointer of every string, and
// the API leaves it unset for the strings it inlines. The integer cast skips the leading spaces.
std::string Pad(const char *text) {
	return std::string(16, ' ') + text;
}

// A two-column chunk: VARCHAR source, INTEGER target.
struct CastChunk {
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_vector_handle source = nullptr;
	duckdb_v2_vector_handle target = nullptr;

	CastChunk(duckdb_v2_factory_handle factory, const std::vector<const char *> &rows) {
		auto varchar = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
		auto integer = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
		duckdb_v2_logical_type_handle types[2] = {varchar, integer};
		auto rc = duckdb_v2_data_chunk_create(factory, types, 2, &chunk, nullptr);
		duckdb_v2_logical_type_destroy(&varchar);
		duckdb_v2_logical_type_destroy(&integer);
		REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &source, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 1, &target, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_vector_set_size(source, rows.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
		for (idx_t i = 0; i < rows.size(); i++) {
			if (!rows[i]) {
				REQUIRE(duckdb_v2_vector_set_null(source, i, nullptr) == DUCKDB_V2_ERROR_NONE);
				continue;
			}
			auto cell = Pad(rows[i]);
			REQUIRE(V2VectorAssignString(source, i, cell.data(), cell.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
		}
	}
	~CastChunk() {
		duckdb_v2_data_chunk_destroy(&chunk);
	}

	// The target rows, -1 standing in for NULL.
	std::vector<int32_t> Target() const {
		duckdb_v2_vector_view view {};
		REQUIRE(duckdb_v2_vector_get_view(target, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
		std::vector<int32_t> result;
		for (idx_t i = 0; i < view.count; i++) {
			auto idx = SelAt(view.sel, i);
			result.push_back(RowValid(view, idx) ? static_cast<const int32_t *>(view.data)[idx] : -1);
		}
		return result;
	}
};

} // namespace

TEST_CASE("V2 vector_cast: a normal cast converts every row or fails", "[capi_v2][vector_cast]") {
	EnvFixture fx;
	{
		CastChunk chunk(fx.factory, {"1", nullptr, "3"});
		bool converted = false;
		REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, &converted,
		                              nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(converted);
		// The target takes the source's size, and NULLs stay NULL.
		REQUIRE(chunk.Target() == std::vector<int32_t> {1, -1, 3});
	}
	{
		CastChunk chunk(fx.factory, {"1", "x", "3"});
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, nullptr,
		                              nullptr, &err) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		REQUIRE(Convert(text).find(Pad("x")) != std::string::npos);
		duckdb_v2_error_info_destroy(&err);
	}
}

TEST_CASE("V2 vector_cast: a try cast turns failed rows into NULL", "[capi_v2][vector_cast]") {
	EnvFixture fx;
	CastChunk chunk(fx.factory, {"1", "x", nullptr, "4"});
	bool converted = true;
	duckdb_v2_error_info_handle cast_error = nullptr;
	REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_TRY, &converted,
	                              &cast_error, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(converted);
	REQUIRE(chunk.Target() == std::vector<int32_t> {1, -1, -1, 4});

	// The error of a failed row is kept.
	REQUIRE(cast_error);
	DUCKDB_V2_ERROR code = DUCKDB_V2_ERROR_NONE;
	REQUIRE(duckdb_v2_error_info_get_code(cast_error, &code) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(code == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	duckdb_v2_str text = {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(cast_error, &text) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_THAT(std::string(text.ptr, text.len), Catch::Matchers::Contains("x"));
	duckdb_v2_error_info_destroy(&cast_error);

	// Every row converting is reported as such, with no error.
	CastChunk clean(fx.factory, {"5", "6"});
	cast_error = nullptr;
	REQUIRE(duckdb_v2_vector_cast(fx.factory, clean.source, clean.target, DUCKDB_V2_CAST_MODE_TRY, &converted,
	                              &cast_error, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(converted);
	REQUIRE(cast_error == nullptr);
	REQUIRE(clean.Target() == std::vector<int32_t> {5, 6});
}

TEST_CASE("V2 vector_cast: nothing the target held carries over", "[capi_v2][vector_cast]") {
	EnvFixture fx;
	CastChunk chunk(fx.factory, {"1", "x", "3"});
	REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_TRY, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk.Target() == std::vector<int32_t> {1, -1, 3});

	// Fix the failing row and cast into the same target: the NULL from before is gone.
	auto fixed = Pad("2");
	REQUIRE(V2VectorAssignString(chunk.source, 1, fixed.data(), fixed.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk.Target() == std::vector<int32_t> {1, 2, 3});

	// An empty source empties the target.
	REQUIRE(duckdb_v2_vector_set_size(chunk.source, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk.Target().empty());
}

TEST_CASE("V2 vector_cast: an instance's factory casts with the built-ins", "[capi_v2][vector_cast]") {
	EnvFixture fx;
	duckdb_v2_factory_handle factory = nullptr;
	REQUIRE(duckdb_v2_instance_get_factory(fx.instance, &factory, nullptr) == DUCKDB_V2_ERROR_NONE);
	CastChunk chunk(factory, {"7", "8"});
	REQUIRE(duckdb_v2_vector_cast(factory, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk.Target() == std::vector<int32_t> {7, 8});
}

TEST_CASE("V2 vector_cast: invalid arguments", "[capi_v2][vector_cast]") {
	EnvFixture fx;
	CastChunk chunk(fx.factory, {"1"});
	REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.source, DUCKDB_V2_CAST_MODE_NORMAL, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_cast(fx.factory, chunk.source, chunk.target, static_cast<DUCKDB_V2_CAST_MODE>(42), nullptr,
	                              nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_cast(nullptr, chunk.source, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_cast(fx.factory, nullptr, chunk.target, DUCKDB_V2_CAST_MODE_NORMAL, nullptr, nullptr,
	                              nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2
