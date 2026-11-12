#include "catch.hpp"
#include "duckdb/common/cow_ptr.hpp"
#include "duckdb/function/function.hpp"

using namespace duckdb;

namespace {

struct CowTestData : public FunctionData {
	explicit CowTestData(idx_t value_p) : value(value_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CowTestData>(value);
	}
	bool Equals(const FunctionData &other) const override {
		return value == other.Cast<CowTestData>().value;
	}

	idx_t value;
};

} // namespace

TEST_CASE("cow_ptr is empty by default", "[cow_ptr]") {
	cow_ptr<FunctionData> ptr;
	REQUIRE(!ptr);
	REQUIRE(ptr == nullptr);
	REQUIRE(ptr.get() == nullptr);
	REQUIRE(ptr.get_mut() == nullptr);

	cow_ptr<FunctionData> null_ptr = nullptr;
	REQUIRE(!null_ptr);
	REQUIRE(ptr.shares_with(null_ptr));
}

TEST_CASE("cow_ptr modifies a uniquely owned object in place", "[cow_ptr]") {
	cow_ptr<FunctionData> ptr = make_uniq<CowTestData>(1);
	REQUIRE(ptr);
	REQUIRE(ptr != nullptr);
	auto original = ptr.get();

	ptr.write().Cast<CowTestData>().value = 2;
	REQUIRE(ptr.get() == original);
	REQUIRE(ptr->Cast<CowTestData>().value == 2);
}

TEST_CASE("cow_ptr copies a shared object before modifying it", "[cow_ptr]") {
	cow_ptr<FunctionData> first = make_uniq<CowTestData>(1);
	auto second = first;
	REQUIRE(first.shares_with(second));

	second.write().Cast<CowTestData>().value = 2;
	REQUIRE(!first.shares_with(second));
	REQUIRE(first->Cast<CowTestData>().value == 1);
	REQUIRE(second->Cast<CowTestData>().value == 2);

	// both are uniquely owned now: further modifications happen in place
	auto first_object = first.get();
	auto second_object = second.get();
	first.write().Cast<CowTestData>().value = 3;
	second.write().Cast<CowTestData>().value = 4;
	REQUIRE(first.get() == first_object);
	REQUIRE(second.get() == second_object);
	REQUIRE(first->Cast<CowTestData>().value == 3);
	REQUIRE(second->Cast<CowTestData>().value == 4);
}

TEST_CASE("cow_ptr stops sharing when the other owner goes away", "[cow_ptr]") {
	cow_ptr<FunctionData> first = make_uniq<CowTestData>(1);
	auto original = first.get();
	{
		auto second = first;
		REQUIRE(first.shares_with(second));
	}
	first.write().Cast<CowTestData>().value = 2;
	REQUIRE(first.get() == original);

	auto moved = std::move(first);
	moved.write().Cast<CowTestData>().value = 3;
	REQUIRE(moved.get() == original);

	auto shared = moved;
	moved.reset();
	REQUIRE(!moved);
	shared.write().Cast<CowTestData>().value = 4;
	REQUIRE(shared.get() == original);
}

TEST_CASE("cow_ptr converts to a read-only optional_ptr", "[cow_ptr]") {
	cow_ptr<FunctionData> ptr = make_uniq<CowTestData>(1);
	optional_ptr<const FunctionData> read_only = ptr;
	REQUIRE(read_only.get() == ptr.get());

	cow_ptr<FunctionData> empty;
	optional_ptr<const FunctionData> empty_read_only = empty;
	REQUIRE(!empty_read_only);
}

TEST_CASE("cow_ptr::make constructs the object in place", "[cow_ptr]") {
	auto ptr = cow_ptr<FunctionData>::make<CowTestData>(idx_t(1));
	REQUIRE(ptr);
	REQUIRE(ptr.read().Cast<CowTestData>().value == 1);

	auto original = ptr.get();
	ptr.write().Cast<CowTestData>().value = 2;
	REQUIRE(ptr.get() == original);

	auto shared = ptr;
	shared.write().Cast<CowTestData>().value = 3;
	REQUIRE(ptr.read().Cast<CowTestData>().value == 2);
	REQUIRE(shared.read().Cast<CowTestData>().value == 3);
}

TEST_CASE("cow_ptr::get_mut mirrors get and detaches", "[cow_ptr]") {
	cow_ptr<FunctionData> first = make_uniq<CowTestData>(1);
	auto second = first;
	auto original = first.get();

	auto mutable_object = second.get_mut();
	REQUIRE(mutable_object != nullptr);
	REQUIRE(mutable_object != original);
	REQUIRE(mutable_object == second.get());
	REQUIRE(first.get() == original);

	// uniquely owned: no copy
	REQUIRE(first.get_mut() == original);
}

TEST_CASE("cow_ptr::from_unique adopts the object", "[cow_ptr]") {
	auto owned = make_uniq<CowTestData>(1);
	auto original = owned.get();
	auto ptr = cow_ptr<FunctionData>::from_unique(std::move(owned));
	REQUIRE(!owned);
	REQUIRE(ptr.get() == original);
	REQUIRE(ptr.get_mut() == original);
}

TEST_CASE("cow_ptr::from_shared copies on write while the shared_ptr is alive", "[cow_ptr]") {
	shared_ptr<CowTestData> owned = make_shared_ptr<CowTestData>(1);
	auto ptr = cow_ptr<FunctionData>::from_shared(owned);
	REQUIRE(ptr.get() == owned.get());

	ptr.write().Cast<CowTestData>().value = 2;
	REQUIRE(ptr.get() != owned.get());
	REQUIRE(owned->value == 1);

	// once the shared_ptr is gone the object is modified in place
	auto second = cow_ptr<FunctionData>::from_shared(std::move(owned));
	auto original = second.get();
	second.write().Cast<CowTestData>().value = 3;
	REQUIRE(second.get() == original);
}
