#include "catch.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("TemporaryMemoryManager preserves minimum reservations", "[storage][temporary_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='64MiB'"));
	REQUIRE_NO_FAIL(con.Query("SET temp_directory=" + Value(TestCreatePath("temporary_memory_minimum")).ToSQLString()));

	SECTION("External processing") {
		REQUIRE_NO_FAIL(con.Query("SET debug_force_external=true"));
	}
	SECTION("Regular processing") {
	}

	auto &manager = TemporaryMemoryManager::Get(context);
	auto state = manager.Register(context);
	constexpr idx_t mib = 1024ULL * 1024ULL;
	state->SetMinimumReservation(96 * mib);
	state->SetRemainingSizeAndUpdateReservation(context, 128 * mib);
	REQUIRE(state->GetReservation() == 96 * mib);

	auto other = manager.Register(context);
	other->SetMinimumReservation(8 * mib);
	other->SetRemainingSizeAndUpdateReservation(context, 128 * mib);
	REQUIRE(other->GetReservation() == 8 * mib);

	state->SetRemainingSizeAndUpdateReservation(context, 80 * mib);
	REQUIRE(state->GetReservation() == 80 * mib);
	state->SetRemainingSizeAndUpdateReservation(context, 48 * mib);
	REQUIRE(state->GetReservation() == 48 * mib);
	state->SetZero();
	REQUIRE(state->GetReservation() == 0);
}

TEST_CASE("TemporaryMemoryManager does not limit states without temporary storage",
          "[storage][temporary_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='64MiB'; SET temp_directory=''"));
	auto state = TemporaryMemoryManager::Get(context).Register(context);
	constexpr idx_t remaining_size = 128ULL * 1024ULL * 1024ULL;
	state->SetRemainingSizeAndUpdateReservation(context, remaining_size);
	REQUIRE(state->GetReservation() == remaining_size);
}

TEST_CASE("TemporaryMemoryManager handles many active states", "[storage][temporary_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto &buffer_pool = DatabaseInstance::GetDatabase(context).GetBufferPool();

	constexpr idx_t memory_limit = 1024ULL * 1024ULL * 1024ULL;
	constexpr idx_t state_count = 80;
	constexpr idx_t initial_reservation = 10ULL * 1024ULL * 1024ULL;
	constexpr idx_t remaining_size = 1024ULL * 1024ULL * 1024ULL;

	buffer_pool.SetLimit(memory_limit, "temporary memory manager test");

	auto &manager = TemporaryMemoryManager::Get(context);
	duckdb::vector<duckdb::unique_ptr<TemporaryMemoryState>> states;
	states.reserve(state_count);
	for (idx_t i = 0; i < state_count; i++) {
		auto state = manager.Register(context);
		state->SetMinimumReservation(initial_reservation);
		state->SetRemainingSize(remaining_size);
		states.push_back(std::move(state));
	}

	REQUIRE_NO_FAIL(con.Query("SET debug_force_external=true"));
	for (auto &state : states) {
		state->UpdateReservation(context);
		REQUIRE(state->GetReservation() == initial_reservation);
	}

	REQUIRE_NO_FAIL(con.Query("SET debug_force_external=false"));
	states.back()->UpdateReservation(context);

	REQUIRE(states.back()->GetReservation() >= initial_reservation);
	REQUIRE(states.back()->GetReservation() <= remaining_size);
}
