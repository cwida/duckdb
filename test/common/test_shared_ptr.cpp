#include "catch.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/pair.hpp"

namespace duckdb {

TEST_CASE("Shared pointer copy assignment preserves aliases and ownership", "[shared_ptr]") {
	auto owner = make_shared_ptr<pair<int32_t, int32_t>>(11, 22);
	shared_ptr<int32_t> first(owner, &owner->first);
	shared_ptr<int32_t> second(owner, &owner->second);
	weak_ptr<int32_t> weak(first);

	SECTION("Different aliases sharing an owner update the stored pointer") {
		first = second;
		REQUIRE(first.get() == second.get());
		REQUIRE(*first == 22);
		REQUIRE(owner.use_count() == 3);
		REQUIRE(*weak.lock() == 11);
	}
	SECTION("Converting assignment preserves aliases") {
		shared_ptr<const int32_t> target(first);
		target = second;
		REQUIRE(target.get() == second.get());
		REQUIRE(*target == 22);
		REQUIRE(owner.use_count() == 4);
	}
	SECTION("Null aliases retain ownership") {
		shared_ptr<int32_t> null_alias(owner, nullptr);
		first = null_alias;
		REQUIRE(!first);
		REQUIRE(first.use_count() == 4);
		owner.reset();
		second.reset();
		null_alias.reset();
		REQUIRE(!weak.expired());
		first.reset();
		REQUIRE(weak.expired());
	}
	SECTION("Empty assignment releases ownership") {
		owner.reset();
		second.reset();
		const shared_ptr<int32_t> empty;
		first = empty;
		REQUIRE(!first);
		REQUIRE(first.use_count() == 0);
		REQUIRE(weak.expired());
	}
	SECTION("Self assignment retains ownership") {
		const auto &same = first;
		first = same;
		REQUIRE(*first == 11);
		REQUIRE(owner.use_count() == 3);
	}
}

TEST_CASE("Equal stored pointers can have different shared ownership", "[shared_ptr]") {
	int32_t value = 42;
	idx_t first_deletions = 0;
	idx_t second_deletions = 0;
	shared_ptr<int32_t> first(&value, [&](int32_t *) { first_deletions++; });
	shared_ptr<int32_t> second(&value, [&](int32_t *) { second_deletions++; });
	weak_ptr<int32_t> old_owner(first);

	first = second;
	REQUIRE(first.get() == &value);
	REQUIRE(first_deletions == 1);
	REQUIRE(second_deletions == 0);
	REQUIRE(old_owner.expired());
	REQUIRE(first.use_count() == 2);
	second.reset();
	REQUIRE(*first == 42);
	first.reset();
	REQUIRE(second_deletions == 1);
}

namespace {
struct SharedAssignmentNode : enable_shared_from_this<SharedAssignmentNode> {
	SharedAssignmentNode(idx_t &destructions_p, int32_t value_p) : destructions(destructions_p), value(value_p) {
	}
	~SharedAssignmentNode() {
		destructions++;
	}
	idx_t &destructions;
	int32_t value;
	shared_ptr<SharedAssignmentNode> next;
};
} // namespace

TEST_CASE("Shared pointer assignment can release an object containing its source", "[shared_ptr]") {
	idx_t destructions = 0;
	auto first = make_shared_ptr<SharedAssignmentNode>(destructions, 11);
	first->next = make_shared_ptr<SharedAssignmentNode>(destructions, 22);
	weak_ptr<SharedAssignmentNode> old_owner(first);
	weak_ptr<SharedAssignmentNode> next_owner(first->next);

	first = first->next;
	REQUIRE(destructions == 1);
	REQUIRE(old_owner.expired());
	REQUIRE(!next_owner.expired());
	REQUIRE(first->value == 22);
	REQUIRE(first.use_count() == 1);
	auto from_this = first->shared_from_this();
	REQUIRE(from_this.get() == first.get());
	REQUIRE(first.use_count() == 2);
	from_this.reset();
	first.reset();
	REQUIRE(destructions == 2);
	REQUIRE(next_owner.expired());
}

TEST_CASE("Unsafe shared pointer copy assignment retains its ownership contract", "[shared_ptr]") {
	unsafe_shared_ptr<int32_t> first(std::make_shared<int32_t>(11));
	unsafe_shared_ptr<int32_t> second(std::make_shared<int32_t>(22));
	first = second;
	REQUIRE(*first == 22);
	REQUIRE(first.use_count() == 2);
	const auto &same = first;
	first = same;
	REQUIRE(first.use_count() == 2);
	second.reset();
	REQUIRE(first.use_count() == 1);
}

} // namespace duckdb
