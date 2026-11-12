#include "catch.hpp"

#include <cerrno>
#include <cstdint>
#include <limits.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

TEST_CASE("BlockAllocator preserves ownership under allocation failures", "[block_allocator]") {
	char path[PATH_MAX];
	auto length = readlink("/proc/self/exe", path, sizeof(path));
	REQUIRE(length > 0);
	REQUIRE(static_cast<size_t>(length) < sizeof(path));
	std::string executable(path, static_cast<size_t>(length));
	auto separator = executable.find_last_of('/');
	REQUIRE(separator != std::string::npos);
	executable.resize(separator + 1);
	executable += "block_allocator_fault_injection";
	INFO(executable);

	auto parent = std::to_string(getpid());
	char *arguments[] = {&executable[0], &parent[0], nullptr};
	pid_t pid = 0;
	auto spawn_result = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, arguments, environ);
	CAPTURE(spawn_result);
	REQUIRE(spawn_result == 0);

	int32_t status = 0;
	pid_t waited;
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited == -1 && errno == EINTR);
	REQUIRE(waited == pid);
	CAPTURE(status);
	REQUIRE(WIFEXITED(status));
	REQUIRE(WEXITSTATUS(status) == 0);
}
