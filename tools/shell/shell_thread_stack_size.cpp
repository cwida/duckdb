#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "shell_thread_stack_size.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <thread>

#if defined(__linux__)
#include <features.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace duckdb_shell {

using namespace duckdb;

namespace {

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 18)
#define DUCKDB_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
#endif
#endif

static bool TryParseThreadStackSize(const string &value, size_t &result) {
	if (value.empty()) {
		return false;
	}
	size_t parsed_value = 0;
	for (auto character : value) {
		if (character < '0' || character > '9') {
			return false;
		}
		auto digit = static_cast<size_t>(character - '0');
		if (parsed_value > (std::numeric_limits<size_t>::max() - digit) / 10) {
			return false;
		}
		parsed_value = parsed_value * 10 + digit;
	}
	if (parsed_value == 0) {
		return false;
	}
	result = parsed_value;
	return true;
}

#ifdef DUCKDB_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
static string PthreadError(const char *function, int error) {
	return string(function) + " failed: " + std::strerror(error);
}

enum class ThreadStackProbeResult : uint8_t { SUCCESS, TOO_SMALL, ERROR };

// glibc's fixed Linux minimum excludes the binary-specific static TLS overhead.
static constexpr size_t GLIBC_PTHREAD_STACK_MIN = 16384;

static void *MinimumThreadStackProbe(void *) {
	return nullptr;
}

static ThreadStackProbeResult TryThreadStackSize(size_t stack_size, string &error) {
	pthread_attr_t attributes;
	auto result = pthread_attr_init(&attributes);
	if (result != 0) {
		error = PthreadError("pthread_attr_init", result);
		return ThreadStackProbeResult::ERROR;
	}

	auto stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		pthread_attr_destroy(&attributes);
		error = "Failed to map a " + to_string(stack_size) + " byte probe stack";
		return ThreadStackProbeResult::ERROR;
	}

	result = pthread_attr_setstack(&attributes, stack, stack_size);
	if (result != 0) {
		pthread_attr_destroy(&attributes);
		munmap(stack, stack_size);
		if (result == EINVAL) {
			return ThreadStackProbeResult::TOO_SMALL;
		}
		error = PthreadError("pthread_attr_setstack", result);
		return ThreadStackProbeResult::ERROR;
	}

	pthread_t probe;
	result = pthread_create(&probe, &attributes, MinimumThreadStackProbe, nullptr);
	auto destroy_result = pthread_attr_destroy(&attributes);
	if (result != 0) {
		munmap(stack, stack_size);
		if (result == EINVAL) {
			return ThreadStackProbeResult::TOO_SMALL;
		}
		error = PthreadError("pthread_create", result);
		return ThreadStackProbeResult::ERROR;
	}
	auto join_result = pthread_join(probe, nullptr);
	munmap(stack, stack_size);
	if (join_result != 0) {
		error = PthreadError("pthread_join", join_result);
		return ThreadStackProbeResult::ERROR;
	}
	if (destroy_result != 0) {
		error = PthreadError("pthread_attr_destroy", destroy_result);
		return ThreadStackProbeResult::ERROR;
	}
	return ThreadStackProbeResult::SUCCESS;
}

static bool GetMinimumThreadStackSize(size_t default_stack_size, size_t page_size, size_t &minimum_stack_size,
                                      string &error) {
	auto lower_page = (GLIBC_PTHREAD_STACK_MIN + page_size - 1) / page_size;
	auto upper_page = default_stack_size / page_size;
	if (upper_page < lower_page) {
		error = "Default pthread stack size is below the glibc minimum";
		return false;
	}
	auto default_probe_result = TryThreadStackSize(upper_page * page_size, error);
	if (default_probe_result != ThreadStackProbeResult::SUCCESS) {
		if (default_probe_result == ThreadStackProbeResult::TOO_SMALL) {
			error = "Unable to create a thread with the default pthread stack size";
		} else {
			error = "Unable to create a thread with the default pthread stack size: " + error;
		}
		return false;
	}

	while (lower_page < upper_page) {
		auto middle_page = lower_page + (upper_page - lower_page) / 2;
		auto probe_result = TryThreadStackSize(middle_page * page_size, error);
		if (probe_result == ThreadStackProbeResult::SUCCESS) {
			upper_page = middle_page;
		} else if (probe_result == ThreadStackProbeResult::TOO_SMALL) {
			lower_page = middle_page + 1;
		} else {
			return false;
		}
	}
	minimum_stack_size = lower_page * page_size;
	return true;
}

static bool SetThreadStackSize(size_t requested_stack_size, string &error) {
	pthread_attr_t default_attributes;
	auto result = pthread_getattr_default_np(&default_attributes);
	if (result != 0) {
		error = PthreadError("pthread_getattr_default_np", result);
		return false;
	}

	size_t default_stack_size;
	result = pthread_attr_getstacksize(&default_attributes, &default_stack_size);
	if (result != 0) {
		pthread_attr_destroy(&default_attributes);
		error = PthreadError("pthread_attr_getstacksize", result);
		return false;
	}
	auto page_size_result = sysconf(_SC_PAGESIZE);
	if (page_size_result <= 0) {
		pthread_attr_destroy(&default_attributes);
		error = "Failed to determine the system page size";
		return false;
	}
	auto page_size = static_cast<size_t>(page_size_result);
	size_t minimum_stack_size;
	if (!GetMinimumThreadStackSize(default_stack_size, page_size, minimum_stack_size, error)) {
		pthread_attr_destroy(&default_attributes);
		return false;
	}
	auto stack_overhead = minimum_stack_size - GLIBC_PTHREAD_STACK_MIN;
	if (requested_stack_size > std::numeric_limits<size_t>::max() - stack_overhead) {
		pthread_attr_destroy(&default_attributes);
		error = "--thread-stack-size is too large";
		return false;
	}
	auto configured_stack_size = requested_stack_size + stack_overhead;

	result = pthread_attr_setstacksize(&default_attributes, configured_stack_size);
	if (result == 0) {
		result = pthread_setattr_default_np(&default_attributes);
	}
	auto destroy_result = pthread_attr_destroy(&default_attributes);
	if (result != 0) {
		error = PthreadError("setting the default pthread stack size", result);
		return false;
	}
	if (destroy_result != 0) {
		error = PthreadError("pthread_attr_destroy", destroy_result);
		return false;
	}

	size_t observed_stack_size = 0;
	int probe_result = 0;
	try {
		std::thread probe([&]() {
			pthread_attr_t probe_attributes;
			probe_result = pthread_getattr_np(pthread_self(), &probe_attributes);
			if (probe_result != 0) {
				return;
			}
			probe_result = pthread_attr_getstacksize(&probe_attributes, &observed_stack_size);
			auto probe_destroy_result = pthread_attr_destroy(&probe_attributes);
			if (probe_result == 0) {
				probe_result = probe_destroy_result;
			}
		});
		probe.join();
	} catch (std::exception &ex) {
		error = string("Failed to create thread stack size probe: ") + ex.what();
		return false;
	}
	if (probe_result != 0) {
		error = PthreadError("reading the probe thread stack size", probe_result);
		return false;
	}

	auto effective_stack_size = observed_stack_size > stack_overhead ? observed_stack_size - stack_overhead : 0;
	auto difference = effective_stack_size > requested_stack_size ? effective_stack_size - requested_stack_size
	                                                              : requested_stack_size - effective_stack_size;
	if (difference > page_size) {
		error = "Thread stack size probe reported " + to_string(observed_stack_size) + " total bytes (" +
		        to_string(effective_stack_size) + " after glibc overhead) after requesting " +
		        to_string(requested_stack_size) + " bytes";
		return false;
	}
	return true;
}
#endif

} // namespace

MetadataResult SetThreadStackSize(ShellState &state, const vector<string> &args) {
	static bool configured = false;
	string error;
	size_t requested_stack_size;
	if (configured) {
		error = "--thread-stack-size may only be specified once";
	} else if (!TryParseThreadStackSize(args[1], requested_stack_size)) {
		error = "--thread-stack-size expected a positive integer size in bytes";
	} else {
#ifdef DUCKDB_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
		SetThreadStackSize(requested_stack_size, error);
#else
		error = "--thread-stack-size is only supported on Linux with glibc 2.18 or newer";
#endif
	}
	if (!error.empty()) {
		state.PrintF(PrintOutput::STDERR, "%s: Error: %s\n", state.program_name, error);
		ShellState::Exit(1);
	}
	configured = true;
	return MetadataResult::SUCCESS;
}

} // namespace duckdb_shell
