/* include/jemalloc/internal/jemalloc_internal_defs.h.  Generated from jemalloc_internal_defs.h.in by configure.  */
#ifndef JEMALLOC_INTERNAL_DEFS_H_
#define JEMALLOC_INTERNAL_DEFS_H_

#include <limits.h>

/*
 * If JEMALLOC_PREFIX is defined via --with-jemalloc-prefix, it will cause all
 * public APIs to be prefixed.  This makes it possible, with some care, to use
 * multiple allocators simultaneously.
 */
#define JEMALLOC_PREFIX  "duckdb_je_"
#define JEMALLOC_CPREFIX "DUCKDB_JE_"

/*
 * Define overrides for non-standard allocator-related functions if they are
 * present on the system.
 */
/* #undef JEMALLOC_OVERRIDE___LIBC_CALLOC */
/* #undef JEMALLOC_OVERRIDE___LIBC_FREE */
/* #undef JEMALLOC_OVERRIDE___LIBC_MALLOC
/* #undef JEMALLOC_OVERRIDE___LIBC_MEMALIGN */
/* #undef JEMALLOC_OVERRIDE___LIBC_REALLOC */
/* #undef JEMALLOC_OVERRIDE___LIBC_VALLOC */
/* #undef JEMALLOC_OVERRIDE___POSIX_MEMALIGN */

/*
 * JEMALLOC_PRIVATE_NAMESPACE is used as a prefix for all library-private APIs.
 * For shared libraries, symbol visibility mechanisms prevent these symbols
 * from being exported, but for static libraries, naming collisions are a real
 * possibility.
 */
#define JEMALLOC_PRIVATE_NAMESPACE duckdb_je_

/*
 * Hyper-threaded CPUs may need a special instruction inside spin loops in
 * order to yield to another virtual CPU.
 */
#if defined(__aarch64__) || defined(__ARM_ARCH)
    #define CPU_SPINWAIT __asm__ volatile("isb")
    #define HAVE_CPU_SPINWAIT 1
#elif defined(__x86_64__) || defined(__i386__)
    #define CPU_SPINWAIT __asm__ volatile("pause")
    #define HAVE_CPU_SPINWAIT 1
#elif defined(__powerpc64__) || defined(__PPC64__) || defined(__POWERPC__)
    #ifdef __APPLE__
        #define CPU_SPINWAIT __asm__ volatile("or r27,r27,r27")
    #else
        #define CPU_SPINWAIT __asm__ volatile("or 27,27,27")
    #endif
    #define HAVE_CPU_SPINWAIT 1
#else
    #define CPU_SPINWAIT
    #define HAVE_CPU_SPINWAIT 0
#endif

/*
 * Number of significant bits in virtual addresses.  This may be less than the
 * total number of bits in a pointer, e.g. on x64, for which the uppermost 16
 * bits are the same as bit 47.
 */
#if INTPTR_MAX == INT64_MAX
#define LG_VADDR 48
#else
#define LG_VADDR 32
#endif

/* Defined if C11 atomics are available. */
#define JEMALLOC_C11_ATOMICS

/* Defined if GCC __atomic atomics are available. */
#ifndef _MSC_VER
#define JEMALLOC_GCC_ATOMIC_ATOMICS
#endif
/* and the 8-bit variant support. */
#define JEMALLOC_GCC_U8_ATOMIC_ATOMICS

/* Defined if GCC __sync atomics are available. */
#ifndef _MSC_VER
#define JEMALLOC_GCC_SYNC_ATOMICS
#endif
/* and the 8-bit variant support. */
#ifndef _MSC_VER
#define JEMALLOC_GCC_U8_SYNC_ATOMICS
#endif

/*
 * Defined if __builtin_clz() and __builtin_clzl() are available.
 */
#ifdef __GNUC__
#define JEMALLOC_HAVE_BUILTIN_CLZ
#endif

/*
 * Defined if os_unfair_lock_*() functions are available, as provided by Darwin.
 */
#if defined(__APPLE__)
#define JEMALLOC_OS_UNFAIR_LOCK
#endif

/* Defined if syscall(2) is usable. */
#ifdef __GLIBC__
#define JEMALLOC_USE_SYSCALL
#endif

/*
 * Defined if secure_getenv(3) is available.
 */
#ifdef __GLIBC__
#undef JEMALLOC_HAVE_SECURE_GETENV
#endif

/*
 * Defined if issetugid(2) is available.
 */
#ifdef __APPLE__
#define JEMALLOC_HAVE_ISSETUGID
#endif

/* Defined if pthread_atfork(3) is available. */
#ifndef _MSC_VER
#define JEMALLOC_HAVE_PTHREAD_ATFORK
#endif

/* Defined if pthread_setname_np(3) is available. */
// #define JEMALLOC_HAVE_PTHREAD_SETNAME_NP

/* Defined if pthread_getname_np(3) is available. */
#ifdef __APPLE__
#define JEMALLOC_HAVE_PTHREAD_GETNAME_NP
#endif

/* Defined if pthread_set_name_np(3) is available. */
/* #undef JEMALLOC_HAVE_PTHREAD_SET_NAME_NP */

/* Defined if pthread_get_name_np(3) is available. */
/* #undef JEMALLOC_HAVE_PTHREAD_GET_NAME_NP */

/*
 * Defined if clock_gettime(CLOCK_MONOTONIC_COARSE, ...) is available.
 */
#ifdef __GLIBC__
#define JEMALLOC_HAVE_CLOCK_MONOTONIC_COARSE
#endif

/*
 * Defined if clock_gettime(CLOCK_MONOTONIC, ...) is available.
 */
#ifdef __GLIBC__
#define JEMALLOC_HAVE_CLOCK_MONOTONIC
#endif

/*
 * Defined if mach_absolute_time() is available.
 */
#ifdef __APPLE__
#define JEMALLOC_HAVE_MACH_ABSOLUTE_TIME
#endif

/*
 * Defined if clock_gettime(CLOCK_REALTIME, ...) is available.
 */
#define JEMALLOC_HAVE_CLOCK_REALTIME

/*
 * Defined if _malloc_thread_cleanup() exists.  At least in the case of
 * FreeBSD, pthread_key_create() allocates, which if used during malloc
 * bootstrapping will cause recursion into the pthreads library.  Therefore, if
 * _malloc_thread_cleanup() exists, use it as the basis for thread cleanup in
 * malloc_tsd.
 */
#if defined(__FreeBSD__)
#define JEMALLOC_MALLOC_THREAD_CLEANUP
#endif

/*
 * Defined if threaded initialization is known to be safe on this platform.
 * Among other things, it must be possible to initialize a mutex without
 * triggering allocation in order for threaded allocation to be safe.
 */
#ifdef __GLIBC__
#define JEMALLOC_THREADED_INIT
#endif

/*
 * Defined if the pthreads implementation defines
 * _pthread_mutex_init_calloc_cb(), in which case the function is used in order
 * to avoid recursive allocation during mutex initialization.
 */
/* #undef JEMALLOC_MUTEX_INIT_CB */

/* Non-empty if the tls_model attribute is supported. */
#define JEMALLOC_TLS_MODEL __attribute__((tls_model("global-dynamic")))

/*
 * JEMALLOC_DEBUG enables assertions and other sanity checks, and disables
 * inline functions.
 */
/* #undef JEMALLOC_DEBUG */

/* JEMALLOC_STATS enables statistics calculation. */
#define JEMALLOC_STATS

/* JEMALLOC_EXPERIMENTAL_SMALLOCX_API enables experimental smallocx API. */
/* #undef JEMALLOC_EXPERIMENTAL_SMALLOCX_API */

/* JEMALLOC_PROF enables allocation profiling. */
/* #undef JEMALLOC_PROF */

/* Use libunwind for profile backtracing if defined. */
/* #undef JEMALLOC_PROF_LIBUNWIND */

/* Use libgcc for profile backtracing if defined. */
/* #undef JEMALLOC_PROF_LIBGCC */

/* Use gcc intrinsics for profile backtracing if defined. */
/* #undef JEMALLOC_PROF_GCC */

/* JEMALLOC_PAGEID enabled page id */
/* #undef JEMALLOC_PAGEID */

/* JEMALLOC_HAVE_PRCTL checks prctl */
#ifdef __GLIBC__
#define JEMALLOC_HAVE_PRCTL
#endif

/*
 * JEMALLOC_DSS enables use of sbrk(2) to allocate extents from the data storage
 * segment (DSS).
 */
#ifdef __GLIBC__
#define JEMALLOC_DSS
#endif

/* Support memory filling (junk/zero). */
#define JEMALLOC_FILL

/* Support utrace(2)-based tracing. */
/* #undef JEMALLOC_UTRACE */

/* Support utrace(2)-based tracing (label based signature). */
/* #undef JEMALLOC_UTRACE_LABEL */

/* Support optional abort() on OOM. */
/* #undef JEMALLOC_XMALLOC */

/* Support lazy locking (avoid locking unless a second thread is launched). */
// #define JEMALLOC_LAZY_LOCK

/*
 * Minimum allocation alignment is 2^LG_QUANTUM bytes (ignoring tiny size
 * classes).
 */
/* #undef LG_QUANTUM */

/* One page is 2^LG_PAGE bytes. */
// ----- DuckDB comment -----
// The page size for jemalloc can always be bigger than the actual system page size
#if INTPTR_MAX != INT64_MAX
#define LG_PAGE 12 // 32-bit systems typically have a 4KB page size
#elif defined(__i386__) || defined(__x86_64__) || defined(__amd64__) ||                                                \
    defined(COMPILER_MSVC) && (defined(_M_IX86) || defined(_M_X64))
#define LG_PAGE 12 // x86 and x86_64 typically have a 4KB page size
#elif defined(__powerpc__) || defined(__ppc__)
#define LG_PAGE 16 // PowerPC architectures often use 64KB page size
#elif defined(__loongarch__)
#define LG_PAGE 14 // LoongArch architectures uses a 16KB page size
#elif defined(__sparc__)
#define LG_PAGE 13 // SPARC architectures usually have an 8KB page size
#elif defined(__aarch64__) || defined(__ARM_ARCH)

// ARM architectures are less well-defined
#if defined(__APPLE__)
#define LG_PAGE 14 // Apple Silicon uses a 16KB page size
#else
#define LG_PAGE 16 // Use max known page size for ARM
#endif

#else
#define LG_PAGE 12 // Default to the most common page size of 4KB
#endif

/* Maximum number of regions in a slab. */
/* #undef CONFIG_LG_SLAB_MAXREGS */

/*
 * One huge page is 2^LG_HUGEPAGE bytes.  Note that this is defined even if the
 * system does not explicitly support huge pages; system calls that require
 * explicit huge page support are separately configured.
 */
#define LG_HUGEPAGE 21

/*
 * If defined, adjacent virtual memory mappings with identical attributes
 * automatically coalesce, and they fragment when changes are made to subranges.
 * This is the normal order of things for mmap()/munmap(), but on Windows
 * VirtualAlloc()/VirtualFree() operations must be precisely matched, i.e.
 * mappings do *not* coalesce/fragment.
 */
#ifndef _MSC_VER
#define JEMALLOC_MAPS_COALESCE
#endif

/*
 * If defined, retain memory for later reuse by default rather than using e.g.
 * munmap() to unmap freed extents.  This is enabled on 64-bit Linux because
 * common sequences of mmap()/munmap() calls will cause virtual memory map
 * holes.
 */
// ----- DuckDB comment -----
// This makes it feasible to run the larger page size (https://github.com/duckdb/duckdb/discussions/11455),
// but it causes DuckDB to retain RSS even after closing the connection, so we have to disable it
#if INTPTR_MAX == INT64_MAX && !defined(__APPLE__)
#define JEMALLOC_RETAIN
#endif

/* TLS is used to map arenas and magazine caches to threads. */
#ifndef __APPLE__
#define JEMALLOC_TLS
#endif

/*
 * Used to mark unreachable code to quiet "end of non-void" compiler warnings.
 * Don't use this directly; instead use unreachable() from util.h
 */
#ifdef _MSC_VER
[[noreturn]] __forceinline void msvc_unreachable() {
	__assume(false);
}
#define JEMALLOC_INTERNAL_UNREACHABLE msvc_unreachable
#else
#define JEMALLOC_INTERNAL_UNREACHABLE __builtin_unreachable
#endif

/*
 * ffs*() functions to use for bitmapping.  Don't use these directly; instead,
 * use ffs_*() from util.h.
 */
#ifdef _MSC_VER
#include "msvc_compat/strings.h"
#define JEMALLOC_INTERNAL_FFSLL ffsll
#define JEMALLOC_INTERNAL_FFSL  ffsl
#define JEMALLOC_INTERNAL_FFS   ffs
#else
#define JEMALLOC_INTERNAL_FFSLL __builtin_ffsll
#define JEMALLOC_INTERNAL_FFSL  __builtin_ffsl
#define JEMALLOC_INTERNAL_FFS   __builtin_ffs
#endif

/*
 * popcount*() functions to use for bitmapping.
 */
#ifdef __GNUC__
#define JEMALLOC_INTERNAL_POPCOUNTL __builtin_popcountl
#define JEMALLOC_INTERNAL_POPCOUNT  __builtin_popcount
#endif

/*
 * If defined, explicitly attempt to more uniformly distribute large allocation
 * pointer alignments across all cache indices.
 */
#if (LG_PAGE == 12)
#define JEMALLOC_CACHE_OBLIVIOUS
#endif

/*
 * If defined, enable logging facilities.  We make this a configure option to
 * avoid taking extra branches everywhere.
 */
/* #undef JEMALLOC_LOG */

/*
 * If defined, use readlinkat() (instead of readlink()) to follow
 * /etc/malloc_conf.
 */
/* #undef JEMALLOC_READLINKAT */

/*
 * If defined, use getenv() (instead of secure_getenv() or
 * alternatives) to access MALLOC_CONF.
 */
/* #undef JEMALLOC_FORCE_GETENV */

/*
 * Darwin (OS X) uses zones to work around Mach-O symbol override shortcomings.
 */
#if defined(__APPLE__)
#define JEMALLOC_ZONE
#endif

/*
 * Methods for determining whether the OS overcommits.
 * JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY: Linux's
 *                                         /proc/sys/vm.overcommit_memory file.
 * JEMALLOC_SYSCTL_VM_OVERCOMMIT: FreeBSD's vm.overcommit sysctl.
 */
#if defined(__linux__)
#define JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
#elif defined(__FreeBSD__)
#define JEMALLOC_SYSCTL_VM_OVERCOMMIT
#endif

/* Defined if madvise(2) is available. */
#define JEMALLOC_HAVE_MADVISE

/*
 * Defined if transparent huge pages are supported via the MADV_[NO]HUGEPAGE
 * arguments to madvise(2).
 */
// #ifdef __GLIBC__
// #define JEMALLOC_HAVE_MADVISE_HUGE
// #endif

/*
 * Methods for purging unused pages differ between operating systems.
 *
 *   madvise(..., MADV_FREE) : This marks pages as being unused, such that they
 *                             will be discarded rather than swapped out.
 *   madvise(..., MADV_DONTNEED) : If JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS is
 *                                 defined, this immediately discards pages,
 *                                 such that new pages will be demand-zeroed if
 *                                 the address region is later touched;
 *                                 otherwise this behaves similarly to
 *                                 MADV_FREE, though typically with higher
 *                                 system overhead.
 */
#define JEMALLOC_PURGE_MADVISE_FREE
#define JEMALLOC_PURGE_MADVISE_DONTNEED
#ifdef __GLIBC__
#define JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
#endif

/* Defined if madvise(2) is available but MADV_FREE is not (x86 Linux only). */
#ifdef __GLIBC__
#define JEMALLOC_DEFINE_MADVISE_FREE
#endif

/*
 * Defined if MADV_DO[NT]DUMP is supported as an argument to madvise.
 */
#ifdef __GLIBC__
#define JEMALLOC_MADVISE_DONTDUMP
#endif

/*
 * Defined if MADV_[NO]CORE is supported as an argument to madvise.
 */
/* #undef JEMALLOC_MADVISE_NOCORE */

/* Defined if mprotect(2) is available. */
#define JEMALLOC_HAVE_MPROTECT

/*
 * Defined if transparent huge pages (THPs) are supported via the
 * MADV_[NO]HUGEPAGE arguments to madvise(2), and THP support is enabled.
 */
/* #undef JEMALLOC_THP */

/* Defined if posix_madvise is available. */
/* #undef JEMALLOC_HAVE_POSIX_MADVISE */

/*
 * Method for purging unused pages using posix_madvise.
 *
 *   posix_madvise(..., POSIX_MADV_DONTNEED)
 */
/* #undef JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED */
/* #undef JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS */

/*
 * Defined if memcntl page admin call is supported
 */
/* #undef JEMALLOC_HAVE_MEMCNTL */

/*
 * Defined if malloc_size is supported
 */
#ifdef __APPLE__
#define JEMALLOC_HAVE_MALLOC_SIZE
#endif

/* Define if operating system has alloca.h header. */
#ifdef __GLIBC__
#define JEMALLOC_HAS_ALLOCA_H
#endif

/* C99 restrict keyword supported. */
#define JEMALLOC_HAS_RESTRICT

/* For use by hash code. */
/* #undef JEMALLOC_BIG_ENDIAN */

/* sizeof(int) == 2^LG_SIZEOF_INT. */
#define LG_SIZEOF_INT 2

/* sizeof(long) == 2^LG_SIZEOF_LONG. */
#if ULONG_MAX > UINT_MAX
#define LG_SIZEOF_LONG 3
#else
#define LG_SIZEOF_LONG 2
#endif

/* sizeof(long long) == 2^LG_SIZEOF_LONG_LONG. */
#define LG_SIZEOF_LONG_LONG 3

/* sizeof(intmax_t) == 2^LG_SIZEOF_INTMAX_T. */
#define LG_SIZEOF_INTMAX_T 3

/* glibc malloc hooks (__malloc_hook, __realloc_hook, __free_hook). */
#ifdef __GLIBC__
#define JEMALLOC_GLIBC_MALLOC_HOOK
#endif

/* glibc memalign hook. */
#ifdef __GLIBC__
#define JEMALLOC_GLIBC_MEMALIGN_HOOK
#endif

/* pthread support */
#define JEMALLOC_HAVE_PTHREAD

/* dlsym() support */
#if defined(__APPLE__) || defined(_GNU_SOURCE)
#define JEMALLOC_HAVE_DLSYM
#endif

/* Adaptive mutex support in pthreads. */
/* #undef JEMALLOC_HAVE_PTHREAD_MUTEX_ADAPTIVE_NP */

/* GNU specific sched_getcpu support */
// #define JEMALLOC_HAVE_SCHED_GETCPU

/* GNU specific sched_setaffinity support */
// #define JEMALLOC_HAVE_SCHED_SETAFFINITY

/* pthread_setaffinity_np support */
/* #undef JEMALLOC_HAVE_PTHREAD_SETAFFINITY_NP */

/*
 * If defined, all the features necessary for background threads are present.
 * Not on macOS: background threads use pthread_cond_wait, which cannot wait
 * on an os_unfair_lock.
 */
#ifndef __APPLE__
#define JEMALLOC_BACKGROUND_THREAD
#endif

/*
 * If defined, jemalloc symbols are not exported (doesn't work when
 * JEMALLOC_PREFIX is not defined).
 */
#define JEMALLOC_EXPORT

/* config.malloc_conf options string. */
#define JEMALLOC_CONFIG_MALLOC_CONF ""

/* If defined, jemalloc takes the malloc/free/etc. symbol names. */
/* #undef JEMALLOC_IS_MALLOC */

/*
 * Defined if strerror_r returns char * if _GNU_SOURCE is defined.
 */
#ifdef __GLIBC__
#define JEMALLOC_STRERROR_R_RETURNS_CHAR_WITH_GNU_SOURCE
#endif

/* Performs additional safety checks when defined. */
/* #undef JEMALLOC_OPT_SAFETY_CHECKS */

/* Is C++ support being built? */
#define JEMALLOC_ENABLE_CXX

/* Performs additional size checks when defined. */
/* #undef JEMALLOC_OPT_SIZE_CHECKS */

/* Allows sampled junk and stash for checking use-after-free when defined. */
/* #undef JEMALLOC_UAF_DETECTION */

/* Darwin VM_MAKE_TAG support */
#if defined(__APPLE__)
#define JEMALLOC_HAVE_VM_MAKE_TAG
#endif

/* If defined, realloc(ptr, 0) defaults to "free" instead of "alloc". */
#ifdef __GLIBC__
#undef JEMALLOC_ZERO_REALLOC_DEFAULT_FREE
#endif

/* If defined, use volatile asm during benchmarks. */
#ifdef __APPLE__
#define JEMALLOC_HAVE_ASM_VOLATILE
#endif

/*
 * If defined, support the use of rdtscp to get the time stamp counter
 * and the processor ID.
 */
/* #undef JEMALLOC_HAVE_RDTSCP */

#include "jemalloc/internal/jemalloc_internal_overrides.h"

#endif /* JEMALLOC_INTERNAL_DEFS_H_ */
