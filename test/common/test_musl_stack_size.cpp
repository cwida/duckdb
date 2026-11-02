#include "catch.hpp"

#include <array>
#include <thread>

TEST_CASE("Check that 7MB can be allocated on stack with musl libc", "[musl_stack_size]") {
#ifndef __MUSL_ENABLED__
	return;
#endif

	std::thread th([] {
		std::array<char, 7 * (1 << 20)> arr;
		volatile char *ptr = arr.data();

		for (size_t i = 0; i < arr.size(); i += (1 << 10)) {
			ptr[i] = 42;
		}
	});

	th.join();
}
