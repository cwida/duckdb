# Run the shell with a heap that is exhausted part-way: every malloc fails once a byte budget is spent. Startup, the
# query and the clean-up afterwards then throw std::bad_alloc; the shell must report it, not abort.

import os
import re
import shutil
import subprocess
import sys

import pytest

from conftest import ShellTest

pytestmark = pytest.mark.skipif(
    not sys.platform.startswith("linux"), reason="the malloc shim is preloaded with LD_PRELOAD"
)

# LD_PRELOAD shim: after FAILMALLOC_AFTER bytes have been requested through the malloc family, every request fails.
# With FAILMALLOC_MAIN_THREAD_FROM=N, every request the main thread makes from its N-th one on fails while the other
# threads keep allocating; FAILMALLOC_REPORT prints the main thread's request count when it first creates a thread.
SHIM_SOURCE = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_memalign)(size_t, size_t);
static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static atomic_size_t total;
static size_t limit = (size_t)-1;
static pid_t main_thread;
static int track_main_thread;
static atomic_size_t main_thread_calls;
static size_t main_thread_from = (size_t)-1;
static atomic_int reported_first_thread;
/* dlsym may allocate while the real functions are still unresolved: serve those requests from a static buffer */
static char boot[65536];
static size_t boot_used;
static int initializing;

static void init(void) {
	if (real_malloc) {
		return;
	}
	initializing = 1;
	real_malloc = dlsym(RTLD_NEXT, "malloc");
	real_calloc = dlsym(RTLD_NEXT, "calloc");
	real_realloc = dlsym(RTLD_NEXT, "realloc");
	real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
	real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
	real_memalign = dlsym(RTLD_NEXT, "memalign");
	real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
	const char *env = getenv("FAILMALLOC_AFTER");
	if (env) {
		limit = strtoull(env, NULL, 10);
	}
	/* the first request comes from the main thread, before any other thread exists */
	main_thread = getpid();
	env = getenv("FAILMALLOC_MAIN_THREAD_FROM");
	if (env) {
		main_thread_from = strtoull(env, NULL, 10);
	}
	track_main_thread = env != NULL || getenv("FAILMALLOC_REPORT") != NULL;
	initializing = 0;
}

static int on_main_thread(void) {
	return (pid_t)syscall(SYS_gettid) == main_thread;
}

static void *boot_alloc(size_t n) {
	void *p = boot + boot_used;
	boot_used += (n + 15) & ~(size_t)15;
	return boot_used <= sizeof(boot) ? p : NULL;
}

static int over(size_t n) {
	size_t t = atomic_fetch_add(&total, n) + n;
	if (t > limit) {
		errno = ENOMEM;
		return 1;
	}
	if (track_main_thread && on_main_thread()) {
		size_t c = atomic_fetch_add(&main_thread_calls, 1) + 1;
		if (c >= main_thread_from) {
			errno = ENOMEM;
			return 1;
		}
	}
	return 0;
}

void *malloc(size_t n) {
	if (initializing) {
		return boot_alloc(n);
	}
	init();
	return over(n) ? NULL : real_malloc(n);
}

void *calloc(size_t a, size_t b) {
	if (initializing) {
		void *p = boot_alloc(a * b);
		if (p) {
			memset(p, 0, a * b);
		}
		return p;
	}
	init();
	return over(a * b) ? NULL : real_calloc(a, b);
}

void *realloc(void *p, size_t n) {
	init();
	return over(n) ? NULL : real_realloc(p, n);
}

int posix_memalign(void **r, size_t al, size_t n) {
	init();
	return over(n) ? ENOMEM : real_posix_memalign(r, al, n);
}

void *aligned_alloc(size_t al, size_t n) {
	init();
	return over(n) ? NULL : real_aligned_alloc(al, n);
}

void *memalign(size_t al, size_t n) {
	init();
	return over(n) ? NULL : real_memalign(al, n);
}

/* with FAILMALLOC_REPORT set, print the main thread's request count the first time it creates a thread */
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg) {
	init();
	if (track_main_thread && getenv("FAILMALLOC_REPORT") && on_main_thread() &&
	    !atomic_exchange(&reported_first_thread, 1)) {
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "failmalloc_first_thread=%zu\n", (size_t)atomic_load(&main_thread_calls));
		write(2, buf, (size_t)n);
	}
	return real_pthread_create(t, a, f, arg);
}

/* with FAILMALLOC_REPORT set, print the bytes requested through the shim at exit */
__attribute__((destructor)) static void report(void) {
	if (getenv("FAILMALLOC_REPORT")) {
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "failmalloc_total_bytes=%zu\n", (size_t)atomic_load(&total));
		write(2, buf, (size_t)n);
	}
}
"""

STARTUP_SQL = "SELECT 42"
# needs far more memory than startup, so every budget below exhausts the heap inside the query
LARGE_SQL = "SELECT " + ",".join(["1"] * 200000)


def run_with_malloc_budget(shell, shim, sql, budget=None, report=False, main_thread_from=None):
    env = dict(os.environ)
    # agent-mode variables of the runner must not switch the shell under test into agent mode
    for key in ShellTest.AGENT_ENVIRONMENT_VARIABLES:
        env.pop(key, None)
    env["LD_PRELOAD"] = shim
    env.pop("FAILMALLOC_AFTER", None)
    env.pop("FAILMALLOC_REPORT", None)
    env.pop("FAILMALLOC_MAIN_THREAD_FROM", None)
    if budget is not None:
        env["FAILMALLOC_AFTER"] = str(budget)
    if main_thread_from is not None:
        env["FAILMALLOC_MAIN_THREAD_FROM"] = str(main_thread_from)
    if report:
        env["FAILMALLOC_REPORT"] = "1"
    # the SQL goes through stdin: the large query does not fit in an argument
    return subprocess.run(
        [shell, "--batch", "--no-init", ":memory:"],
        input=sql.encode(),
        capture_output=True,
        env=env,
        timeout=300,
    )


@pytest.fixture(scope="module")
def malloc_shim(tmp_path_factory):
    compiler = os.environ.get("CC")
    if not compiler or not shutil.which(compiler):
        compiler = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
    if not compiler:
        pytest.skip("no C compiler to build the malloc shim")
    directory = tmp_path_factory.mktemp("failmalloc")
    source = directory / "failmalloc.c"
    source.write_text(SHIM_SOURCE)
    shim = directory / "failmalloc.so"
    build = subprocess.run(
        [compiler, "-shared", "-fPIC", "-O2", "-o", str(shim), str(source), "-ldl"],
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        pytest.skip("cannot build the malloc shim: " + build.stderr)
    return str(shim)


# the bytes a plain `SELECT 42` run requests through the shim, so the budgets below scale with the build
@pytest.fixture(scope="module")
def startup_bytes(request, malloc_shim):
    shell = request.config.getoption("--shell-binary")
    if not shell:
        raise ValueError("Please provide a shell binary path to the tester, using '--shell-binary <path_to_cli>'")
    result = run_with_malloc_budget(shell, malloc_shim, STARTUP_SQL, report=True)
    assert result.returncode == 0, result.stderr
    assert b"42" in result.stdout, result.stdout
    match = re.search(rb"failmalloc_total_bytes=(\d+)", result.stderr)
    if not match or int(match.group(1)) == 0:
        pytest.skip("the malloc shim does not intercept the allocations of this shell binary")
    return int(match.group(1))


# the requests the main thread makes before it starts the first worker thread: the failures below begin around there
@pytest.fixture(scope="module")
def first_thread_calls(request, malloc_shim):
    shell = request.config.getoption("--shell-binary")
    result = run_with_malloc_budget(shell, malloc_shim, STARTUP_SQL, report=True)
    assert result.returncode == 0, result.stderr
    match = re.search(rb"failmalloc_first_thread=(\d+)", result.stderr)
    if not match:
        pytest.skip("this shell binary starts no worker thread")
    return int(match.group(1))


def check_out_of_memory_exit(result):
    assert result.returncode == 1, (result.returncode, result.stderr)
    assert b"Out of Memory" in result.stderr, result.stderr
    assert b"terminate called" not in result.stderr, result.stderr


# the heap runs out during startup, the query, the output or the clean-up
@pytest.mark.parametrize("fraction", [0.1, 0.25, 0.5, 0.75, 0.9, 0.95])
def test_malloc_failure_startup(shell, malloc_shim, startup_bytes, fraction):
    result = run_with_malloc_budget(shell, malloc_shim, STARTUP_SQL, budget=int(startup_bytes * fraction))
    check_out_of_memory_exit(result)


# the heap runs out after the connection exists, while a large query is parsed, bound or executed
@pytest.mark.parametrize("factor", [1.05, 1.25, 1.5, 2.0, 3.0])
def test_malloc_failure_large_query(shell, malloc_shim, startup_bytes, factor):
    result = run_with_malloc_budget(shell, malloc_shim, LARGE_SQL, budget=int(startup_bytes * factor))
    check_out_of_memory_exit(result)


# the heap runs out while the result is rendered or the shell shuts down: whether the query still completes depends
# on the exact budget, but the shell must never abort
@pytest.mark.parametrize("fraction", [0.96, 0.97, 0.98, 0.99])
def test_malloc_failure_tail(shell, malloc_shim, startup_bytes, fraction):
    result = run_with_malloc_budget(shell, malloc_shim, STARTUP_SQL, budget=int(startup_bytes * fraction))
    assert result.returncode in (0, 1), (result.returncode, result.stderr)
    assert b"terminate called" not in result.stderr, result.stderr
    if result.returncode == 1:
        assert b"Out of Memory" in result.stderr, result.stderr


# the heap runs out for the main thread while the scheduler launches its worker threads, from the allocation that
# precedes the first thread to the ones after it: the scheduler must keep every thread it started, not abort
@pytest.mark.parametrize("offset", range(61))
def test_malloc_failure_thread_launch(shell, malloc_shim, first_thread_calls, offset):
    result = run_with_malloc_budget(shell, malloc_shim, STARTUP_SQL, main_thread_from=first_thread_calls + offset)
    check_out_of_memory_exit(result)
